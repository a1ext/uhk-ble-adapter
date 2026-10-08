#include "ble_hid.h"
#include <Arduino.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>
#include <esp_gatt_common_api.h>
#include <esp_system.h>
#include <array>
#include "log.h"
#include "ota.h"

// BLEHIDDevice creates the HID service with room for 40 attribute handles:
// 9 are taken by the mandatory characteristics, an input report needs 4
// (value + CCCD + report reference), output / feature reports need 3
static const int HID_SERVICE_HANDLES   = 40;
static const int HID_BASE_HANDLES      = 9;
static const int AGENT_SERVICE_HANDLES = 20;

// Encryption normally completes right after connecting; if the auth event
// never arrives, start sending anyway after this delay
static const uint32_t SECURE_FALLBACK_MS = 3000;

static const char *NVS_NAMESPACE = "bleslots";

struct Slot {
    bool          used;    // a host is paired on this slot
    esp_bd_addr_t host;
    uint8_t       gen;     // bumped when a new host replaces the old one: fresh address
};
static Slot             slots[BLE_SLOTS];
static volatile uint8_t curSlot = 0;
static Preferences      nvs;

static BLEServer         *server;
static BLEHIDDevice      *hid;
static BLECharacteristic *inputChr[256];
static BleHidWriteCb      writeCb;
static bool started = false;

static volatile bool     enabled = false;
static volatile bool     connected = false;
static volatile bool     secured = false;
static volatile bool     congested = false;
static volatile bool     advertising = false;
static bool              advDiscoverable = false;
static volatile bool     pairing = false;
static uint32_t          pairingUntilMs = 0;
static std::vector<std::array<uint8_t, 6>> bondsBeforePairing; // to find the new host's bond
static volatile uint16_t connId = 0;
static volatile uint16_t mtu = 23;
static volatile uint16_t connInterval = 0;
static volatile uint32_t connectedAtMs = 0;
static volatile uint8_t  authFailReason = 0;
static volatile bool     pairingRejected = false;
static volatile bool     hostPending = false;   // a host completed pairing / encryption
static volatile uint8_t  hostPendingSlot;
static esp_bd_addr_t     hostPendingAddr;
static esp_bd_addr_t     peerAddr;
static int               sendableBaseline = -1;

// ---------------- Slots ----------------
static void saveSlot(uint8_t s)
{
    char key[8];
    snprintf(key, sizeof(key), "s%u", s);
    nvs.putBytes(key, &slots[s], sizeof(Slot));
}

void ble_hid_init()
{
    nvs.begin(NVS_NAMESPACE, false);
    curSlot = min<uint8_t>(nvs.getUChar("cur", 0), BLE_SLOTS - 1);
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        char key[8];
        snprintf(key, sizeof(key), "s%u", s);
        memset(&slots[s], 0, sizeof(Slot));
        if (nvs.isKey(key) && nvs.getBytesLength(key) == sizeof(Slot))
            nvs.getBytes(key, &slots[s], sizeof(Slot));
    }
}

// Pairing a new host on a used slot advertises the next generation's address,
// committed once the new host has paired (cancel = back to the old host)
static uint8_t slotGen(uint8_t s)
{
    return slots[s].gen + (pairing && s == curSlot && slots[s].used ? 1 : 0);
}

// Static random address derived from the chip MAC, the slot and its generation
static void slotAddress(uint8_t s, esp_bd_addr_t out)
{
    static const uint8_t SLOT_MASK[] = {0x55, 0xAA, 0xFF, 0x33, 0xCC};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    memcpy(out, mac, 6);
    out[0] |= 0xC0; // static random address: two most significant bits set
    out[4] ^= slotGen(s);
    out[5] ^= SLOT_MASK[s % sizeof(SLOT_MASK)];
}

static void slotName(uint8_t s, char *buf, size_t size)
{
    snprintf(buf, size, "%s %u", BLE_DEVICE_NAME, s + 1);
}

// Identity addresses of all bonded hosts (what bonds are keyed by - phones
// and Macs connect with changing private addresses)
static std::vector<std::array<uint8_t, 6>> bondList()
{
    std::vector<std::array<uint8_t, 6>> out;
    int n = esp_ble_get_bond_device_num();
    if (n <= 0)
        return out;
    auto *list = (esp_ble_bond_dev_t *)malloc(n * sizeof(esp_ble_bond_dev_t));
    if (!list)
        return out;
    esp_ble_get_bond_device_list(&n, list);
    for (int i = 0; i < n; i++) {
        std::array<uint8_t, 6> a;
        memcpy(a.data(), list[i].bd_addr, 6);
        out.push_back(a);
    }
    free(list);
    return out;
}

// Name + address of the active slot; advertising must be stopped
static void applySlotIdentity()
{
    char name[32];
    slotName(curSlot, name, sizeof(name));
    esp_ble_gap_set_device_name(name);
    esp_bd_addr_t addr;
    slotAddress(curSlot, addr);
    server->getAdvertising()->setDeviceAddress(addr, BLE_ADDR_TYPE_RANDOM);
}

// Moves to the active slot's (new) identity: drops the host and advertising,
// ble_hid_poll() advertises again if there is someone to serve
static void restartOnSlot()
{
    if (!started)
        return;
    if (advertising) {
        BLEDevice::stopAdvertising();
        advertising = false;
    }
    applySlotIdentity();
    if (connected)
        esp_ble_gap_disconnect(peerAddr);
}

// Discoverable (LE General Discoverable flag) only in pairing mode: without
// it hosts do not list the adapter, but a bonded host still reconnects
static void configureAdvertising(bool discoverable)
{
    char name[32];
    slotName(curSlot, name, sizeof(name));
    BLEAdvertisementData data;
    data.setFlags(discoverable ? (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)
                               : ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    data.setAppearance(HID_KEYBOARD);
    data.setCompleteServices(BLEUUID((uint16_t)0x1812));
    data.setName(name); // in the advertisement itself, no scan response needed
    server->getAdvertising()->setAdvertisementData(data);
    advDiscoverable = discoverable;
}

// ---------------- Bluedroid callbacks (BTC task) ----------------
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *s, esp_ble_gatts_cb_param_t *p) override
    {
        connId = p->connect.conn_id;
        memcpy(peerAddr, p->connect.remote_bda, sizeof(peerAddr));
        mtu = 23;
        connInterval = 0;
        secured = false;
        congested = false;
        advertising = false; // legacy advertising stops on connection
        sendableBaseline = -1;
        connectedAtMs = millis();

        // The Arduino BLE library does not persist CCCDs across connections,
        // and a bonded host does not rewrite them on reconnect: force notify on
        for (auto *c : inputChr) {
            if (!c)
                continue;
            auto *cccd = (BLE2902 *)c->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
            if (cccd)
                cccd->setNotifications(true);
        }
        connected = true;

        // Keyboard: short interval, no slave latency (powered from USB anyway)
        s->updateConnParams(p->connect.remote_bda, 6, 12, 0, 400);
    }

    void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *) override
    {
        connected = false;
        secured = false;
        congested = false;
    }
};

class SecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t) override {}
    bool onConfirmPIN(uint32_t) override { return true; }

    // A pairing request: accepted in pairing mode, otherwise only from the
    // slot's own host (e.g. it lost its keys)
    bool onSecurityRequest() override
    {
        const Slot &s = slots[curSlot];
        bool ok = pairing || (s.used && memcmp(s.host, peerAddr, sizeof(esp_bd_addr_t)) == 0);
        if (!ok)
            pairingRejected = true;
        return ok;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t r) override
    {
        secured = r.success;
        authFailReason = r.success ? 0 : r.fail_reason;
        if (r.success) {
            memcpy(hostPendingAddr, r.bd_addr, sizeof(esp_bd_addr_t));
            hostPendingSlot = curSlot;
            hostPending = true;
        }
    }
};

class HostWriteCallbacks : public BLECharacteristicCallbacks {
public:
    HostWriteCallbacks(HidReportType type, uint8_t id) : type(type), id(id) {}

    void onWrite(BLECharacteristic *c) override
    {
        if (!writeCb)
            return;
        std::string v = c->getValue();
        writeCb(type, id, (const uint8_t *)v.data(), v.size());
    }

private:
    HidReportType type;
    uint8_t id;
};

static void gattsHandler(esp_gatts_cb_event_t event, esp_gatt_if_t, esp_ble_gatts_cb_param_t *p)
{
    if (event == ESP_GATTS_CONGEST_EVT)
        congested = p->congest.congested;
    else if (event == ESP_GATTS_MTU_EVT)
        mtu = p->mtu.mtu;
}

static void gapHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    if (event == ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT && p->update_conn_params.status == ESP_BT_STATUS_SUCCESS)
        connInterval = p->update_conn_params.conn_int;
}

// ---------------- HID services ----------------
// Report characteristic (0x2A4D) with its Report Reference descriptor
static BLECharacteristic *makeReport(BLEService *svc, HidReportType type, uint8_t refId, uint8_t internalId)
{
    uint32_t props = type == HID_INPUT  ? BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
                   : type == HID_OUTPUT ? BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
                                              | BLECharacteristic::PROPERTY_WRITE_NR
                                        : BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
    BLECharacteristic *c = svc->createCharacteristic((uint16_t)0x2a4d, props);
    c->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
    if (type == HID_INPUT) {
        BLE2902 *cccd = new BLE2902();
        cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
        c->addDescriptor(cccd);
    }
    BLEDescriptor *ref = new BLEDescriptor(BLEUUID((uint16_t)0x2908));
    ref->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
    uint8_t refVal[] = {refId, (uint8_t)type};
    ref->setValue(refVal, sizeof(refVal));
    c->addDescriptor(ref);
    if (type != HID_INPUT)
        c->setCallbacks(new HostWriteCallbacks(type, internalId));
    return c;
}

// Second HID service for the UHK Agent command channel (report ID 0 inside).
// inst_id 1 is essential: the Arduino BLE library matches the service-created
// event by UUID + inst_id, so a second 0x1812 with inst_id 0 hijacks the main
// HID service's handle and scrambles its characteristics (reads then go
// unanswered - Windows sees the device as GATT-unreachable).
static void makeAgentService(const std::vector<uint8_t> &agentMap, const std::vector<BleHidReportDef> &reports)
{
    BLEService *svc = server->createService(BLEUUID((uint16_t)0x1812), AGENT_SERVICE_HANDLES, 1);
    BLECharacteristic *info = svc->createCharacteristic((uint16_t)0x2a4a, BLECharacteristic::PROPERTY_READ);
    uint8_t infoVal[] = {0x11, 0x01, 0x00, 0x02}; // HID 1.11, country 0, normally connectable
    info->setValue(infoVal, sizeof(infoVal));
    BLECharacteristic *map = svc->createCharacteristic((uint16_t)0x2a4b, BLECharacteristic::PROPERTY_READ);
    map->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
    map->setValue((uint8_t *)agentMap.data(), agentMap.size());
    svc->createCharacteristic((uint16_t)0x2a4c, BLECharacteristic::PROPERTY_WRITE_NR); // control point
    for (auto &r : reports) {
        if (!r.agent)
            continue;
        BLECharacteristic *c = makeReport(svc, r.type, 0, r.id);
        if (r.type == HID_INPUT)
            inputChr[r.id] = c;
        std::vector<uint8_t> zero(r.bytes, 0);
        c->setValue(zero.data(), zero.size());
    }
    svc->start();
}

// ---------------- Public API ----------------
void ble_hid_begin(const std::vector<uint8_t> &reportMap,
                   const std::vector<BleHidReportDef> &reports,
                   const std::vector<uint8_t> &agentMap, BleHidWriteCb onWrite,
                   uint16_t pnpVid, uint16_t pnpPid, uint16_t pnpVersion)
{
    if (started)
        return;
    writeCb = onWrite;

    int handles = HID_BASE_HANDLES;
    for (auto &r : reports)
        if (!r.agent)
            handles += r.type == HID_INPUT ? 4 : 3;
    if (handles > HID_SERVICE_HANDLES)
        logf("BLE: WARNING %d HID handles needed, service has %d", handles, HID_SERVICE_HANDLES);
    if (reportMap.size() > 512)
        logf("BLE: WARNING report map %u bytes > 512 (HOGP limit)", (unsigned)reportMap.size());

    char name[32];
    slotName(curSlot, name, sizeof(name));
    BLEDevice::init(name);
    BLEDevice::setMTU(BLE_LOCAL_MTU);
    BLEDevice::setCustomGattsHandler(gattsHandler);
    BLEDevice::setCustomGapHandler(gapHandler);

    server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    hid = new BLEHIDDevice(server);

    // Main HID service: keyboard, mouse, controls (first, for hosts that use
    // only one HID service - Android)
    for (auto &r : reports) {
        if (r.agent)
            continue;
        BLECharacteristic *c = makeReport(hid->hidService(), r.type, r.id, r.id);
        if (r.type == HID_INPUT)
            inputChr[r.id] = c;
        std::vector<uint8_t> zero(r.bytes, 0);
        c->setValue(zero.data(), zero.size());
    }

    hid->manufacturer()->setValue(BLE_MANUFACTURER);
    // BLEHIDDevice::pnp() stores the 16-bit fields big-endian, PnP ID is
    // little-endian: pass them byte-swapped (Windows showed A837:0300)
    hid->pnp(0x02, __builtin_bswap16(pnpVid), __builtin_bswap16(pnpPid), __builtin_bswap16(pnpVersion));
    hid->hidInfo(0x00, 0x02); // country 0, normally connectable
    hid->reportMap((uint8_t *)reportMap.data(), reportMap.size());

    BLESecurity *sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec->setCapability(ESP_IO_CAP_NONE); // "Just Works" pairing
    // Take the host's identity key (resolves phones' random addresses), but do
    // not send ours: hosts must bind to the slot's own address, not the chip's
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK);
    BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

    hid->startServices();
    hid->setBatteryLevel(100);
    if (!agentMap.empty())
        makeAgentService(agentMap, reports);
    ota_create_service(server);

    server->getAdvertising()->setScanResponse(false); // everything fits in the advertisement
    applySlotIdentity();

    started = true;
    logf("BLE: HID service up as %04X:%04X, %u reports, report map %u bytes%s, %d bonded host(s)",
         pnpVid, pnpPid, (unsigned)reports.size(), (unsigned)reportMap.size(),
         agentMap.empty() ? "" : " + Agent service", esp_ble_get_bond_device_num());
}

bool ble_hid_started()
{
    return started;
}

void ble_hid_set_enabled(bool on)
{
    enabled = on;
    if (!on)
        pairing = false;
    if (!started)
        return;
    if (on) {
        ble_hid_poll(); // starts advertising
    } else {
        if (advertising) {
            BLEDevice::stopAdvertising();
            advertising = false;
        }
        if (connected)
            esp_ble_gap_disconnect(peerAddr);
    }
}

void ble_hid_poll()
{
    if (!started)
        return;

    static bool     wasConnected = false, wasSecured = false;
    static uint16_t loggedMtu = 0, loggedInterval = 0;
    static uint8_t  loggedFail = 0;

    if (connected != wasConnected) {
        wasConnected = connected;
        if (connected)
            logf("BLE: host %02X:%02X:%02X:%02X:%02X:%02X connected", peerAddr[0], peerAddr[1],
                 peerAddr[2], peerAddr[3], peerAddr[4], peerAddr[5]);
        else
            logf("BLE: host disconnected");
        loggedMtu = loggedInterval = 0;
    }
    if (secured != wasSecured) {
        wasSecured = secured;
        if (secured)
            logf("BLE: link encrypted (bonded)");
    }
    if (authFailReason != loggedFail) {
        loggedFail = authFailReason;
        if (loggedFail)
            logf("BLE: pairing failed, reason 0x%02X - remove the device on the host and pair again",
                 loggedFail);
    }
    if (pairingRejected) {
        pairingRejected = false;
        logf("BLE: pairing refused - hold \"Pair\" on the screen first");
        if (connected)
            esp_ble_gap_disconnect(peerAddr);
    }
    if (hostPending) {
        hostPending = false;
        // Encryption also completes on every reconnect of a bonded host -
        // with a fresh private address for phones / Macs. Only a pairing in
        // pairing mode may change the slot.
        if (pairing && hostPendingSlot == curSlot) {
            uint8_t s = curSlot;
            Slot &sl = slots[s];
            // The new bond's key is the host's identity address
            std::array<uint8_t, 6> host;
            memcpy(host.data(), hostPendingAddr, 6);
            for (auto &b : bondList()) {
                bool known = false;
                for (auto &o : bondsBeforePairing)
                    known |= o == b;
                if (!known)
                    host = b;
            }
            // Bluedroid keeps one bond per host: pairing it on this slot
            // replaced the keys it had for another slot
            for (uint8_t o = 0; o < BLE_SLOTS; o++) {
                if (o != s && slots[o].used && !memcmp(slots[o].host, host.data(), 6)) {
                    slots[o].used = false;
                    saveSlot(o);
                    logf("BLE: host moved from slot %u to slot %u", o + 1, s + 1);
                }
            }
            if (sl.used) {
                // A new host replaced the old one: its bond goes, and the
                // slot keeps the trial address the new host paired on
                if (memcmp(sl.host, host.data(), 6))
                    esp_ble_remove_bond_device(sl.host);
                sl.gen++;
            }
            sl.used = true;
            memcpy(sl.host, host.data(), 6);
            saveSlot(s);
            pairing = false; // done; the advertised address stays (gen committed above)
            advDiscoverable = false;
            logf("BLE: slot %u paired with %02X:%02X:%02X:%02X:%02X:%02X", s + 1, host[0], host[1],
                 host[2], host[3], host[4], host[5]);
        }
    }
    // The window closes on time, but not under a host that is connected and
    // still pairing (Windows reads the services first, then pairs)
    if (pairing && (int32_t)(millis() - pairingUntilMs) >= 0) {
        bool busy = connected && !secured && millis() - connectedAtMs < 30000;
        if (!busy) {
            logf("BLE: pairing mode timed out");
            ble_hid_stop_pairing();
        }
    }
    if (connected && mtu != loggedMtu) {
        loggedMtu = mtu;
        logf("BLE: MTU %u%s", mtu, mtu < 32 ? " - too small for the NKRO report!" : "");
    }
    if (connected && connInterval && connInterval != loggedInterval) {
        loggedInterval = connInterval;
        logf("BLE: connection interval %u.%02u ms", connInterval * 125 / 100, connInterval * 125 % 100);
    }

    // Advertise only if there is someone to serve: the slot's host (hidden)
    // or a new device in pairing mode (discoverable)
    bool want = enabled && !connected && (pairing || slots[curSlot].used);
    if (advertising && (!want || advDiscoverable != pairing)) {
        BLEDevice::stopAdvertising();
        advertising = false;
    }
    if (want && !advertising) {
        configureAdvertising(pairing);
        BLEDevice::startAdvertising();
        advertising = true;
        char name[32];
        slotName(curSlot, name, sizeof(name));
        if (pairing)
            logf("BLE: discoverable as \"%s\" - pair it on the new device", name);
        else
            logf("BLE: waiting for the slot %u host", curSlot + 1);
    }
}

bool ble_hid_ready()
{
    return connected && (secured || millis() - connectedAtMs > SECURE_FALLBACK_MS);
}

bool ble_hid_can_send()
{
    if (!ble_hid_ready() || congested)
        return false;
    // Keep at most a couple of notifications queued in the controller, so
    // input latency does not pile up when the host polls slower than the
    // keyboard reports (newer reports wait in the bridge queue and merge)
    int sendable = esp_ble_get_cur_sendable_packets_num(connId);
    if (sendable > sendableBaseline)
        sendableBaseline = sendable;
    return sendableBaseline - sendable < BLE_MAX_IN_FLIGHT;
}

void ble_hid_send(uint8_t id, const uint8_t *data, uint16_t len)
{
    BLECharacteristic *c = inputChr[id];
    if (!c)
        return;
    c->setValue((uint8_t *)data, len);
    c->notify();
}

void ble_hid_select_slot(uint8_t slot)
{
    if (slot >= BLE_SLOTS || slot == curSlot)
        return;
    pairing = false; // pairing mode belongs to the slot it was started on
    curSlot = slot;
    nvs.putUChar("cur", slot);
    logf("BLE: slot %u selected", slot + 1);
    restartOnSlot();
}

void ble_hid_start_pairing()
{
    if (started)
        bondsBeforePairing = bondList();
    pairing = true;
    pairingUntilMs = millis() + PAIRING_WINDOW_MS;
    logf("BLE: slot %u pairing mode for %u s", curSlot + 1, (unsigned)(PAIRING_WINDOW_MS / 1000));
    restartOnSlot(); // drops the current host, a used slot gets a trial address
}

void ble_hid_stop_pairing()
{
    if (!pairing)
        return;
    pairing = false;
    restartOnSlot(); // back to the slot's own address, its host can reconnect
}

void ble_hid_forget_pairings()
{
    int n = 0;
    pairing = false;
    if (started) {
        auto bonds = bondList();
        n = bonds.size();
        for (auto &b : bonds)
            esp_ble_remove_bond_device(b.data());
    }
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        slots[s].used = false;
        slots[s].gen++;
        saveSlot(s);
    }
    logf("BLE: %d pairing(s) removed - also remove the old \"%s\" devices on the hosts", n,
         BLE_DEVICE_NAME);
    restartOnSlot();
}

void ble_hid_get_status(BleHidStatus &st)
{
    st.started   = started;
    st.enabled   = enabled;
    st.connected = connected;
    st.ready     = ble_hid_ready();
    st.mtu       = mtu;
    st.interval  = connInterval;
    st.slot      = curSlot;
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        st.slotUsed[s] = slots[s].used;
        memcpy(st.slotHost[s], slots[s].host, 6);
    }
    int32_t left = (int32_t)(pairingUntilMs - millis());
    st.pairingLeftMs = pairing && left > 0 ? left : 0;
}
