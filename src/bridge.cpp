#include "bridge.h"

static volatile uint32_t lastInputMs = 0; // last key / click / motion (USB task), 0 = none yet
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/semphr.h>
#include <vector>
#include "ble_hid.h"
#include "config.h"
#include "hid_desc.h"
#include "log.h"
#include "stats.h"
#include "usb_hid_host.h"

static const char *NVS_NAMESPACE = "hidmap";
static const char *NVS_KEY_DESC  = "desc";
// Bumped whenever the BLE report map / GATT layout built from the same
// keyboard changes: hosts cache it, so pairings are dropped on upgrade.
// 3: + keyboard VID:PID, Agent channel in its own HID service
// 4: Report ID inside the top-level collection, PnP ID byte order fixed
// 5: passkey pairing (MITM) - Just Works bonds of older firmware are dropped
static const uint8_t BLOB_VERSION = 5;
static const int   MAX_IFACES    = 8;
static const int   MAX_REPORT    = 64;
static const int   Q_CAP         = 64;
static const int   Q_HIGH_WATER  = 48;   // above this the USB endpoints are not re-polled
static const uint32_t RESTART_DELAY_MS = 2000;

// ---------------- Report map ----------------
// Interface descriptors as (bInterfaceNumber, report descriptor) pairs
typedef std::vector<std::pair<uint8_t, std::vector<uint8_t>>> DescSet;

struct IfaceMap {
    uint8_t     num;          // bInterfaceNumber
    bool        forward;      // parsed OK; vendor-defined only if it is the Agent channel
    HidDescInfo info;
    uint8_t     idMap[256];   // USB report ID -> BLE report ID ([0] for interfaces without IDs)
};

struct BleRef {               // indexed by BLE report ID
    int8_t  iface = -1;       // index into ifmaps
    uint8_t usbId = 0;        // report ID on USB, 0 = interface uses no IDs
    const HidReportLayout *in = nullptr; // input layout (merging)
    std::vector<uint8_t> absMask;        // input bits that must match to merge
    std::vector<uint8_t> prev;           // last input report (statistics), empty = not counted
};

static Preferences nvs;
static std::vector<uint8_t> mapBlob;  // serialized keyboard the current map was built from
static uint16_t mapVid, mapPid;       // keyboard's USB VID:PID (BLE PnP ID for Agent)
static std::vector<IfaceMap> ifmaps;  // immutable once mapValid is set
static BleRef bleRefs[256];
static std::vector<uint8_t> reportMap;  // main HID service
static std::vector<uint8_t> agentMap;   // Agent HID service (keyboard's descriptor as is)
static std::vector<BleHidReportDef> bleReportDefs;
static volatile bool mapValid = false;
static int8_t  agentIface = -1;       // index into ifmaps of the UHK Agent command channel
static uint8_t agentInId = 0;         // internal ID of its input report
static bool    forgetOnStart = false; // cached map from older firmware: hosts must re-pair

// UHK Agent commands over BLE (UHK firmware usb_protocol_handler.h)
enum AgentAccess : uint8_t {
    AGENT_ALWAYS,    // read-only status, Agent polls it
    AGENT_UNLOCKED,  // only after "Allow" on the adapter's screen
    AGENT_NEVER,     // keyboard firmware updates: keyboard on the PC by cable
};

static AgentAccess agentAccess(const uint8_t *cmd, uint16_t len)
{
    switch (cmd[0]) {
    case 0x00: // GetDeviceProperty
    case 0x09: // GetDeviceState
    case 0x0b: // GetDebugBuffer
    case 0x0c: // GetAdcValue
    case 0x0e: // GetModuleProperty
    case 0x0f: // GetSlaveI2cErrors
    case 0x12: // GetVariable
    case 0x1b: // IsPaired
    case 0x22: // GetModuleFlashState
        return AGENT_ALWAYS;
    case 0x04: // ReadConfig: buffer 0, the hardware config (layout, IDs), is no secret
        return len > 1 && cmd[1] == 0 ? AGENT_ALWAYS : AGENT_UNLOCKED;
    case 0x01: // Reenumerate (bootloader / BusPal)
    case 0x02: // JumpToModuleBootloader
    case 0x03: // SendKbootCommandToModule
    case 0x20: // WriteModuleFirmware
    case 0x21: // FlashModule
        return AGENT_NEVER;
    }
    // Everything else reads the user config (macros may hold passwords),
    // changes it, runs macros or otherwise changes the keyboard's state
    return AGENT_UNLOCKED;
}

// What UHK Agent looks for: vendor page 0xFF00 usage 1 (old firmware 0x80 / 0x81)
static bool isAgentChannel(const HidDescInfo &info)
{
    return (info.tlcUsagePage == 0xFF00 && info.tlcUsage == 0x01)
        || (info.tlcUsagePage == 0x0080 && info.tlcUsage == 0x81);
}

// ---------------- Runtime state ----------------
static volatile bool usbReady = false;
static int8_t usbToMap[MAX_IFACES];
static int8_t mapToUsb[MAX_IFACES];
static char   usbProduct[32];
static volatile bool bleStartPending = false;
static volatile bool restartPending = false;
static uint32_t restartAtMs = 0;
static const char *volatile note = nullptr;
static volatile uint32_t statForwarded = 0, statMerged = 0, statDropped = 0;
static volatile uint32_t agentFwRefused = 0, agentLockedRefused = 0;
// UHK Agent config access, granted on the screen to the connected host: lasts
// AGENT_UNLOCK_MS after its last such command, ends when the host disconnects
static volatile bool     agentUnlocked = false;
static volatile uint32_t agentUnlockUntilMs = 0;
static volatile bool     agentAsking = false;    // refused while locked: prompt on the screen
static volatile uint32_t agentAskedAtMs = 0;
static const char *NOTE_NOT_KEYBOARD = "USB device is not a keyboard (bootloader?)";
static const char *NOTE_FW_REFUSED   = "keyboard firmware update refused: plug the UHK into the PC";

// Output / feature reports written by the BLE host, re-sent to the keyboard
// after it (re)enumerates: e.g. Caps Lock LED, scroll resolution multiplier
struct HostReport {
    HidReportType type;
    uint8_t id, len;
    uint8_t data[MAX_REPORT];
};
static std::vector<HostReport> hostReports;
static SemaphoreHandle_t hostLock;

// ---------------- Input report FIFO ----------------
struct QItem {
    uint8_t id, len;
    uint8_t data[MAX_REPORT];
};
static QItem q[Q_CAP];
static int   qHead = 0;
static volatile int qCount = 0;
static SemaphoreHandle_t qLock;
static TaskHandle_t senderTask;

// [version][vid][pid][count] then per interface [num][len16][descriptor]
static std::vector<uint8_t> serialize(const DescSet &ds, uint16_t vid, uint16_t pid)
{
    std::vector<uint8_t> b;
    b.push_back(BLOB_VERSION);
    b.push_back(vid & 0xFF);
    b.push_back(vid >> 8);
    b.push_back(pid & 0xFF);
    b.push_back(pid >> 8);
    b.push_back(ds.size());
    for (auto &d : ds) {
        b.push_back(d.first);
        b.push_back(d.second.size() & 0xFF);
        b.push_back(d.second.size() >> 8);
        b.insert(b.end(), d.second.begin(), d.second.end());
    }
    return b;
}

static bool deserialize(const std::vector<uint8_t> &b, DescSet &ds, uint16_t &vid, uint16_t &pid)
{
    ds.clear();
    if (b.size() < 6 || b[0] != BLOB_VERSION)
        return false;
    vid = b[1] | (b[2] << 8);
    pid = b[3] | (b[4] << 8);
    size_t pos = 6;
    for (int i = 0; i < b[5]; i++) {
        if (pos + 3 > b.size())
            return false;
        uint8_t num = b[pos];
        size_t len = b[pos + 1] | (b[pos + 2] << 8);
        pos += 3;
        if (pos + len > b.size())
            return false;
        ds.push_back({num, std::vector<uint8_t>(b.begin() + pos, b.begin() + pos + len)});
        pos += len;
    }
    return pos == b.size();
}

static const char *typeName(HidReportType t)
{
    return t == HID_INPUT ? "in" : t == HID_OUTPUT ? "out" : "feat";
}

static bool buildMap(const DescSet &ds)
{
    ifmaps.clear();
    ifmaps.reserve(ds.size()); // layouts are referenced by pointer below
    reportMap.clear();
    agentMap.clear();
    bleReportDefs.clear();
    for (auto &r : bleRefs)
        r = BleRef();
    agentIface = -1;
    agentInId = 0;

    unsigned nextId = 1;
    for (auto &d : ds) {
        ifmaps.emplace_back();
        IfaceMap &m = ifmaps.back();
        m.num = d.first;
        memset(m.idMap, 0, sizeof(m.idMap));
        bool ok = hid_parse(d.second.data(), d.second.size(), m.info);
        bool agent = ok && BLE_AGENT_CHANNEL && isAgentChannel(m.info);
        m.forward = ok && !m.info.reports.empty() && (!m.info.isVendor() || agent);
        if (agent && m.forward)
            agentIface = ifmaps.size() - 1;
        logf("MAP: IF%u usage %04X:%04X, %u report(s)%s%s", m.num, m.info.tlcUsagePage,
             m.info.tlcUsage, (unsigned)m.info.reports.size(), m.info.usesIds ? ", with IDs" : "",
             !ok ? " - PARSE ERROR, skipped" : agent ? " - UHK Agent channel"
             : m.info.isVendor() ? " - vendor, skipped" : "");
        if (!m.forward)
            continue;

        if (!m.info.usesIds)
            m.idMap[0] = nextId++;
        else
            for (auto &r : m.info.reports)
                if (r.id && !m.idMap[r.id])
                    m.idMap[r.id] = nextId++;
        if (nextId > 255) {
            logf("MAP: too many report IDs");
            return false;
        }
        // The Agent channel keeps its descriptor (no report IDs) in its own
        // HID service; its internal ID only routes reports inside the bridge
        if ((int)ifmaps.size() - 1 == agentIface && !m.info.usesIds)
            agentMap = d.second;
        else
            hid_append_remapped(d.second.data(), d.second.size(), m.info.usesIds, m.idMap, reportMap);
    }

    for (size_t i = 0; i < ifmaps.size(); i++) {
        IfaceMap &m = ifmaps[i];
        if (!m.forward)
            continue;
        for (auto &r : m.info.reports) {
            uint8_t bid = m.info.usesIds ? m.idMap[r.id] : m.idMap[0];
            if (!bid || !r.bytes() || r.bytes() > MAX_REPORT) {
                logf("MAP: IF%u %s report %u (%u bytes) skipped", m.num, typeName(r.type), r.id, r.bytes());
                continue;
            }
            bleReportDefs.push_back({r.type, bid, r.bytes(), (int)i == agentIface && !agentMap.empty()});
            BleRef &ref = bleRefs[bid];
            ref.iface = i;
            ref.usbId = m.info.usesIds ? r.id : 0;
            if (r.type == HID_INPUT) {
                if ((int)i == agentIface && !agentInId)
                    agentInId = bid;
                ref.in = &r;
                bool counted = !r.keys.empty();
                for (auto &f : r.rel)
                    counted |= f.xyMask != 0;
                if (counted)
                    ref.prev.assign(r.bytes(), 0);
                ref.absMask.assign(r.bytes(), 0xFF);
                for (auto &f : r.rel)
                    for (unsigned k = 0; k < f.count; k++)
                        for (unsigned b = 0; b < f.bitSize; b++) {
                            unsigned bit = f.bitOffset + k * f.bitSize + b;
                            if (bit / 8 < ref.absMask.size())
                                ref.absMask[bit / 8] &= ~(1 << (bit % 8));
                        }
            }
            logf("MAP: IF%u %s id %u -> BLE id %u, %u bytes%s", m.num, typeName(r.type), r.id, bid,
                 r.bytes(), r.rel.empty() ? "" : ", relative axes");
        }
    }
    if (bleReportDefs.empty()) {
        logf("MAP: no reports to forward");
        return false;
    }
    logf("MAP: BLE report map %u bytes", (unsigned)reportMap.size());
    if (!agentMap.empty())
        logf("MAP: UHK Agent channel in its own HID service (%u bytes)", (unsigned)agentMap.size());
    return true;
}

// ---------------- FIFO with merging ----------------
static uint32_t getBits(const uint8_t *d, unsigned off, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++, off++)
        if (d[off / 8] & (1 << (off % 8)))
            v |= 1u << i;
    return v;
}

static void setBits(uint8_t *d, unsigned off, unsigned size, uint32_t v)
{
    for (unsigned i = 0; i < size; i++, off++) {
        if (v & (1u << i))
            d[off / 8] |= 1 << (off % 8);
        else
            d[off / 8] &= ~(1 << (off % 8));
    }
}

static int32_t fieldValue(const uint8_t *d, unsigned off, const HidRelField &f)
{
    uint32_t v = getBits(d, off, f.bitSize);
    if (f.logMin < 0 && f.bitSize < 32 && (v & (1u << (f.bitSize - 1))))
        v |= ~0u << f.bitSize; // sign-extend
    return (int32_t)v;
}

// Merges `in` into `tail` if both carry the same absolute state (buttons)
static bool tryMerge(QItem &tail, const uint8_t *in, const BleRef &ref)
{
    if (!ref.in || ref.in->rel.empty())
        return false;
    for (size_t i = 0; i < ref.absMask.size(); i++)
        if ((tail.data[i] ^ in[i]) & ref.absMask[i])
            return false;
    unsigned bits = ref.in->bytes() * 8;
    for (auto &f : ref.in->rel) {
        for (unsigned k = 0; k < f.count; k++) {
            unsigned off = f.bitOffset + k * f.bitSize;
            if (off + f.bitSize > bits)
                break;
            int64_t sum = (int64_t)fieldValue(tail.data, off, f) + fieldValue(in, off, f);
            sum = constrain(sum, (int64_t)f.logMin, (int64_t)f.logMax);
            setBits(tail.data, off, f.bitSize, (uint32_t)sum);
        }
    }
    return true;
}

static void qPush(uint8_t id, const uint8_t *payload, uint16_t len)
{
    const BleRef &ref = bleRefs[id];
    QItem item;
    item.id = id;
    item.len = ref.in->bytes();
    memset(item.data, 0, item.len);
    memcpy(item.data, payload, min<uint16_t>(len, item.len));

    xSemaphoreTake(qLock, portMAX_DELAY);
    bool merged = false;
    if (qCount) {
        QItem &tail = q[(qHead + qCount - 1) % Q_CAP];
        merged = tail.id == id && tryMerge(tail, item.data, ref);
    }
    if (merged) {
        statMerged++;
    } else {
        if (qCount == Q_CAP) { // only reachable if the keyboard ignores backpressure
            qHead = (qHead + 1) % Q_CAP;
            qCount--;
            statDropped++;
        }
        q[(qHead + qCount) % Q_CAP] = item;
        qCount++;
    }
    xSemaphoreGive(qLock);
    xTaskNotifyGive(senderTask);
}

static bool qPop(QItem &item)
{
    xSemaphoreTake(qLock, portMAX_DELAY);
    bool ok = qCount > 0;
    if (ok) {
        item = q[qHead];
        qHead = (qHead + 1) % Q_CAP;
        qCount--;
    }
    xSemaphoreGive(qLock);
    return ok;
}

static void qClear()
{
    xSemaphoreTake(qLock, portMAX_DELAY);
    qHead = qCount = 0;
    xSemaphoreGive(qLock);
}

static void senderTaskFn(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        while (qCount) {
            if (!ble_hid_ready()) {
                qClear();
                break;
            }
            if (!ble_hid_can_send()) {
                vTaskDelay(1);
                continue;
            }
            QItem item;
            if (!qPop(item))
                break;
            ble_hid_send(item.id, item.data, item.len);
            statForwarded++;
        }
    }
}

// Releases everything on the host (all-zero input reports: no keys, no
// buttons, no motion) when the keyboard goes away mid-press
static void sendReleaseAll()
{
    if (!ble_hid_ready())
        return;
    static const uint8_t zero[MAX_REPORT] = {0};
    for (int id = 1; id < 256; id++)
        if (bleRefs[id].in)
            qPush(id, zero, bleRefs[id].in->bytes());
}

// ---------------- The keyboard's backlight timeout ----------------
// Asked of the keyboard itself over the Agent channel, 1.5 s after it came up:
// ReadConfig (0x04) of the validated user config (buffer 2), two 63-byte reads of
// its head, parsed like the UHK firmware's parse_config.c: keyBacklightFadeOutTimeout
// (seconds) with data model >= 8, ledsFadeTimeout with 6..7. The replies are the
// next Agent input reports and are not forwarded to the host. The screen can then
// go dark together with the backlight (anim.h kbdIdleS 0 = this value).
static const uint8_t  PROBE_CHUNK = 62;                    // the 63-byte report less its status byte
static const uint32_t PROBE_DELAY_MS = 1500, PROBE_REPLY_MS = 1000, PROBE_QUIET_MS = 2000;
static uint8_t  probeStep = 0;                             // 0 idle, 1 / 2 chunk n requested, 3 done, 4 given up
static uint32_t probeDueMs = 0, probeSentMs = 0, agentCmdMs = 0;
static uint8_t  probeBuf[2 * PROBE_CHUNK];
static uint16_t kbdBacklightS = 0;                         // 0 = unknown
static char     probeWhy[40];                              // why it failed, for the log

static bool parseBacklightTimeout(const uint8_t *b, size_t n, uint16_t &timeoutS)
{
    size_t p = 0;
    auto u8 = [&](uint8_t &v) { if (p + 1 > n) return false; v = b[p++]; return true; };
    auto u16 = [&](uint16_t &v) { if (p + 2 > n) return false; v = b[p] | b[p + 1] << 8; p += 2; return true; };
    auto skip = [&](size_t k) { if (p + k > n) return false; p += k; return true; };
    uint16_t major, minor, patch, len;
    uint8_t len8;
    if (!u16(major) || !u16(minor) || !u16(patch) || major < 6 || major > 30)
        return false;
    if (!skip(4))                                          // userConfigLength (u32 since v6)
        return false;
    if (!u8(len8) || (len8 == 0xFF ? !u16(len) : (len = len8, false)) || len > 64 || !skip(len))
        return false;                                      // device name
    if (!skip(2))                                          // doubleTapSwitchLayerTimeout
        return false;
    if (major < 8)                                         // 3 brightness bytes, then ledsFadeTimeout
        return skip(3) && u16(timeoutS);
    // perKeyRgbPresent, backlightingMode, 8 key action colours (+ Device since v9)
    if (!skip(1 + 1 + 8 * 3 + (major >= 9 ? 3 : 0)))
        return false;
    if (!skip(10))                                         // mouse kinetic properties
        return false;
    // v7: secondary role strategy (1 + 2 + 2 + 2 + 1 + 1 + 1), two axis skews (floats),
    // diagonalSpeedCompensation, doubletapTimeout, keystrokeDelay
    if (!skip(10 + 8 + 1 + 2 + 2))
        return false;
    // v8: 4 brightness bytes, display fade-outs (2 x u16), then keyBacklightFadeOutTimeout
    return skip(4 + 2 + 2) && u16(timeoutS);
}

static void probeRequest(uint8_t chunk)                    // USB / loop task
{
    uint8_t req[PROBE_CHUNK] = {0x04 /* ReadConfig */, 2 /* validated user config */, PROBE_CHUNK,
                                (uint8_t)(chunk * PROBE_CHUNK), 0};
    probeStep = chunk + 1;
    probeSentMs = millis();
    usb_hid_host_set_report(mapToUsb[agentIface], HID_OUTPUT, 0, req, sizeof(req));
}

// An Agent input report while a chunk is awaited: true = it was the reply (consumed)
static bool probeTakeReply(const uint8_t *data, uint16_t len)
{
    if (probeStep != 1 && probeStep != 2)
        return false;
    uint8_t chunk = probeStep - 1;
    if (len < 1 + PROBE_CHUNK || data[0] != 0) {           // UsbStatusCode_Success
        snprintf(probeWhy, sizeof(probeWhy), "status %u, %u bytes", data[0], len);
        probeStep = 4;
        return true;
    }
    memcpy(probeBuf + chunk * PROBE_CHUNK, data + 1, PROBE_CHUNK);
    if (chunk == 0) {
        probeRequest(1);
        return true;
    }
    uint16_t t;
    if (parseBacklightTimeout(probeBuf, sizeof(probeBuf), t) && t <= 3600) {
        kbdBacklightS = t;
        probeStep = 3;
    } else {
        snprintf(probeWhy, sizeof(probeWhy), "config v%u.%u not parsed", probeBuf[0] | probeBuf[1] << 8,
                 probeBuf[2] | probeBuf[3] << 8);
        probeStep = 4;
    }
    return true;
}

static void probeSchedule()
{
    probeStep = 0;
    kbdBacklightS = 0;
    probeDueMs = millis() + PROBE_DELAY_MS;
}

static void probeLoop()                                    // Arduino loop
{
    static uint8_t logged = 0;
    if (probeStep == 0 && probeDueMs && usbReady && agentIface >= 0 && mapToUsb[agentIface] >= 0
        && (int32_t)(millis() - probeDueMs) >= 0 && millis() - agentCmdMs > PROBE_QUIET_MS) {
        probeDueMs = 0;
        logged = 0;
        probeRequest(0);
    } else if ((probeStep == 1 || probeStep == 2) && millis() - probeSentMs > PROBE_REPLY_MS) {
        snprintf(probeWhy, sizeof(probeWhy), "no reply to chunk %u", probeStep - 1);
        probeStep = 4;
    }
    if (probeStep >= 3 && logged != probeStep) {
        logged = probeStep;
        if (probeStep == 3)
            logf("Keyboard: backlight fades after %u s", kbdBacklightS);
        else
            logf("Keyboard: backlight timeout not read (%s)", probeWhy);
    }
}

uint16_t bridge_keyboard_backlight_timeout_s()
{
    return kbdBacklightS;
}

// Diagnostics for `ble_anim.py info`: the probe's state and the head of the config
const char *bridge_keyboard_probe_note()
{
    static char note[128];
    int n = snprintf(note, sizeof(note), "%s: ",
                     probeStep == 3 ? "read" : probeStep == 4 ? probeWhy : probeStep ? "pending" : "not started");
    for (int i = 0; i < 40 && n < (int)sizeof(note) - 3; i++)
        n += snprintf(note + n, sizeof(note) - n, "%02x", probeBuf[i]);
    return note;
}

// ---------------- Host -> keyboard (output / feature reports) ----------------
static void forwardHostReport(const HostReport &h)
{
    const BleRef &ref = bleRefs[h.id];
    if (ref.iface < 0 || mapToUsb[ref.iface] < 0)
        return;
    usb_hid_host_set_report(mapToUsb[ref.iface], h.type, ref.usbId, h.data, h.len);
}

// UHK Agent command from the BLE host: passed straight to the keyboard, never
// cached / replayed. Refused commands get an InvalidCommand reply.
static void onAgentCommand(HidReportType type, uint8_t id, const uint8_t *data, uint16_t len)
{
    if (type != HID_OUTPUT || !len)
        return; // commands are output reports
    AgentAccess a = agentAccess(data, len);
    if (a == AGENT_UNLOCKED && agentUnlocked && (int32_t)(millis() - agentUnlockUntilMs) < 0) {
        agentUnlockUntilMs = millis() + AGENT_UNLOCK_MS; // in use: keep it open
        a = AGENT_ALWAYS;
    }
    if (a != AGENT_ALWAYS) {
        static const uint8_t refused[MAX_REPORT] = {1}; // UsbStatusCode_InvalidCommand
        if (agentInId)
            qPush(agentInId, refused, sizeof(refused));
        if (a == AGENT_NEVER) {
            note = NOTE_FW_REFUSED;
            agentFwRefused++;
        } else {
            agentAskedAtMs = millis();
            agentAsking = true;
            agentLockedRefused++;
        }
        return;
    }
    HostReport h{type, id, (uint8_t)min<uint16_t>(len, MAX_REPORT), {}};
    memcpy(h.data, data, h.len);
    agentCmdMs = millis(); // the backlight probe keeps out of the host's way
    if (usbReady)
        forwardHostReport(h);
}

// BTC task context: no logging here (small stack)
static void onHostWrite(HidReportType type, uint8_t id, const uint8_t *data, uint16_t len)
{
    if (agentIface >= 0 && bleRefs[id].iface == agentIface) {
        onAgentCommand(type, id, data, len);
        return;
    }

    HostReport h;
    h.type = type;
    h.id = id;
    h.len = min<uint16_t>(len, MAX_REPORT);
    memcpy(h.data, data, h.len);

    xSemaphoreTake(hostLock, portMAX_DELAY);
    bool found = false;
    for (auto &e : hostReports)
        if (e.type == type && e.id == id) {
            e = h;
            found = true;
        }
    if (!found)
        hostReports.push_back(h);
    xSemaphoreGive(hostLock);

    if (usbReady)
        forwardHostReport(h);
}

// ---------------- USB callbacks (USB client task) ----------------
static uint32_t onDeviceReady(const UsbHidDevice &dev)
{
    // A device without a keyboard collection - e.g. the UHK in its bootloader
    // after a firmware update was started anyway - must not replace the map
    bool keyboard = false;
    for (auto &f : dev.ifaces) {
        HidDescInfo info;
        if (hid_parse(f.reportDesc.data(), f.reportDesc.size(), info)
                && info.tlcUsagePage == 0x01 && info.tlcUsage == 0x06)
            keyboard = true;
    }
    if (!keyboard) {
        logf("USB: %04X:%04X has no keyboard interface - not forwarded", dev.vid, dev.pid);
        note = NOTE_NOT_KEYBOARD;
        return 0;
    }
    if (note == NOTE_NOT_KEYBOARD)
        note = nullptr;
    strlcpy(usbProduct, dev.product.c_str(), sizeof(usbProduct));

    DescSet ds;
    for (auto &f : dev.ifaces)
        ds.push_back({f.num, f.reportDesc});
    std::vector<uint8_t> blob = serialize(ds, dev.vid, dev.pid);

    if (!mapValid) {
        // First keyboard seen: its descriptors become the BLE report map
        nvs.putBytes(NVS_KEY_DESC, blob.data(), blob.size());
        mapBlob = blob;
        mapVid = dev.vid;
        mapPid = dev.pid;
        if (!buildMap(ds)) {
            note = "unsupported keyboard HID layout";
            return 0;
        }
        mapValid = true;
        bleStartPending = true;
    } else if (blob != mapBlob) {
        // Keyboard firmware changed its descriptors (e.g. NKRO <-> 6KRO in UHK
        // Agent) or another keyboard was attached. Hosts cache the report map
        // per pairing, so the GATT database is rebuilt after a restart and the
        // old pairings are dropped.
        logf("MAP: keyboard (%04X:%04X) HID descriptors changed - restarting", dev.vid, dev.pid);
        nvs.putBytes(NVS_KEY_DESC, blob.data(), blob.size());
        note = "HID layout changed: restart, re-pair on the host";
        restartAtMs = millis();
        restartPending = true;
        return 0;
    }

    memset(usbToMap, -1, sizeof(usbToMap));
    memset(mapToUsb, -1, sizeof(mapToUsb));
    uint32_t mask = 0;
    for (size_t i = 0; i < dev.ifaces.size() && i < MAX_IFACES; i++) {
        for (size_t k = 0; k < ifmaps.size() && k < MAX_IFACES; k++) {
            if (ifmaps[k].num != dev.ifaces[i].num)
                continue;
            usbToMap[i] = k;
            mapToUsb[k] = i;
            if (ifmaps[k].forward && dev.ifaces[i].epIn)
                mask |= 1u << i;
        }
    }
    usbReady = true;
    probeSchedule(); // ask the keyboard for its backlight timeout in a moment

    xSemaphoreTake(hostLock, portMAX_DELAY);
    std::vector<HostReport> pending = hostReports;
    xSemaphoreGive(hostLock);
    for (auto &h : pending)
        forwardHostReport(h);
    return mask;
}

// Typing statistics: new key / button presses (bits going 0 -> 1, codes
// that were not in the previous report) and pointer travel
static void countReport(BleRef &ref, const uint8_t *data, uint16_t len)
{
    const HidReportLayout &r = *ref.in;
    uint8_t now[MAX_REPORT] = {0};
    memcpy(now, data, min<uint16_t>(len, r.bytes()));
    const uint8_t *was = ref.prev.data();
    unsigned bits = r.bytes() * 8;
    uint32_t keys = 0, clicks = 0;
    for (auto &f : r.keys) {
        for (unsigned k = 0; k < f.count; k++) {
            unsigned off = f.bitOffset + k * f.bitSize;
            if (off + f.bitSize > bits)
                break;
            uint32_t v = getBits(now, off, f.bitSize);
            if (!f.array) {
                if (v && !getBits(was, off, 1))
                    (f.kind == HID_BUTTONS ? clicks : keys)++;
                continue;
            }
            if (v <= 3) // none / keyboard error codes (rollover)
                continue;
            bool held = false;
            for (unsigned j = 0; j < f.count && !held; j++)
                held = getBits(was, f.bitOffset + j * f.bitSize, f.bitSize) == v;
            if (!held)
                keys++;
        }
    }
    int32_t xy[2] = {0, 0};
    unsigned axes = 0;
    for (auto &f : r.rel)
        for (unsigned k = 0; k < f.count && k < 8 && axes < 2; k++)
            if (f.xyMask & (1 << k) && f.bitOffset + (k + 1) * f.bitSize <= bits)
                xy[axes++] = fieldValue(now, f.bitOffset + k * f.bitSize, f);
    memcpy(ref.prev.data(), now, ref.prev.size());
    if (keys || clicks || xy[0] || xy[1]) {
        stats_add(keys, clicks, sqrtf((float)xy[0] * xy[0] + (float)xy[1] * xy[1]));
        lastInputMs = millis() | 1; // never 0 once something came
    }
}

static void onInputReport(uint8_t ifaceIdx, const uint8_t *data, uint16_t len)
{
    if (!usbReady || ifaceIdx >= MAX_IFACES || usbToMap[ifaceIdx] < 0)
        return;
    const IfaceMap &m = ifmaps[usbToMap[ifaceIdx]];
    if (!m.forward)
        return;
    if (usbToMap[ifaceIdx] == agentIface && probeTakeReply(data, len))
        return; // our own ReadConfig reply, not the host's
    uint8_t id;
    if (m.info.usesIds) {
        if (!len)
            return;
        id = m.idMap[data[0]];
        data++;
        len--;
    } else {
        id = m.idMap[0];
    }
    if (!id || !bleRefs[id].in)
        return;
    if (!bleRefs[id].prev.empty())
        countReport(bleRefs[id], data, len);
    if (ble_hid_ready()) // no host: drop, the next report carries the full state anyway
        qPush(id, data, len);
}

static void onDeviceGone()
{
    bool was = usbReady;
    usbReady = false;
    usbProduct[0] = 0;
    if (was)
        sendReleaseAll();
}

static bool canAcceptReport()
{
    return qCount < Q_HIGH_WATER;
}

// ---------------- Public API ----------------
void bridge_begin()
{
    qLock = xSemaphoreCreateMutex();
    hostLock = xSemaphoreCreateMutex();
    memset(usbToMap, -1, sizeof(usbToMap));
    memset(mapToUsb, -1, sizeof(mapToUsb));
    ble_hid_init();

    nvs.begin(NVS_NAMESPACE, false);
    size_t n = nvs.getBytesLength(NVS_KEY_DESC);
    if (n) {
        std::vector<uint8_t> blob(n);
        nvs.getBytes(NVS_KEY_DESC, blob.data(), n);
        DescSet ds;
        if (blob[0] != BLOB_VERSION) {
            // Map built by an older firmware: the new one differs, so hosts
            // paired with the old GATT database must pair again
            logf("MAP: cached layout from an older firmware dropped - hosts must pair again");
            nvs.remove(NVS_KEY_DESC);
            forgetOnStart = true;
        } else if (deserialize(blob, ds, mapVid, mapPid) && buildMap(ds)) {
            mapBlob = blob;
            mapValid = true;
        }
    }
    logf("MAP: %s", mapValid ? "using the cached keyboard layout"
                             : "none yet - attach the keyboard");

    xTaskCreatePinnedToCore(senderTaskFn, "ble_tx", 4096, nullptr, 4, &senderTask, 1);
}

static void startBle()
{
    if (!ble_hid_started()) {
        // With the Agent channel the BLE device must look like the keyboard
        // itself (Agent matches USB VID:PID + usage page)
        bool agent = !agentMap.empty();
        ble_hid_begin(reportMap, bleReportDefs, agentMap, onHostWrite,
                      agent ? mapVid : BLE_PNP_VID, agent ? mapPid : BLE_PNP_PID, 0x0100);
        if (forgetOnStart) {
            forgetOnStart = false;
            ble_hid_forget_pairings();
        }
    }
    ble_hid_set_enabled(true);
}

void bridge_start()
{
    if (mapValid)
        startBle();
    UsbHidHostCallbacks cb = {onDeviceReady, onInputReport, onDeviceGone, canAcceptReport};
    usb_hid_host_begin(cb);
}

void bridge_loop()
{
    if (bleStartPending) {
        bleStartPending = false;
        startBle();
    }
    ble_hid_poll();
    probeLoop();

    static uint32_t loggedFw = 0, loggedLocked = 0;
    if (agentFwRefused != loggedFw) {
        loggedFw = agentFwRefused;
        logf("Agent: firmware update command refused over BLE - plug the keyboard into the PC");
    }
    if (agentLockedRefused != loggedLocked) {
        loggedLocked = agentLockedRefused;
        logf("Agent: config access refused - allow it on the screen");
    }
    if (agentUnlocked && (!ble_hid_ready() || (int32_t)(millis() - agentUnlockUntilMs) >= 0)) {
        agentUnlocked = false;
        logf("Agent: config access locked again%s", ble_hid_ready() ? " (idle)" : "");
    }
    if (agentAsking && (!ble_hid_ready() || millis() - agentAskedAtMs > AGENT_PROMPT_MS))
        agentAsking = false;

    if (restartPending && millis() - restartAtMs > RESTART_DELAY_MS) {
        if (ble_hid_started())
            ble_hid_forget_pairings();
        stats_save();
        delay(200);
        esp_restart();
    }
}

void bridge_agent_allow(bool allow)
{
    agentAsking = false;
    if (allow && !ble_hid_ready())
        return;
    agentUnlockUntilMs = millis() + AGENT_UNLOCK_MS;
    agentUnlocked = allow;
    logf("Agent: config access %s", allow ? "allowed - retry in UHK Agent" : "locked");
}

uint32_t bridge_last_input_ms()
{
    return lastInputMs;
}

void bridge_get_status(BridgeStatus &st)
{
    st.usbConnected = usbReady;
    strlcpy(st.usbProduct, usbProduct, sizeof(st.usbProduct));
    st.mapValid = mapValid;
    st.forwarded = statForwarded;
    st.merged = statMerged;
    st.dropped = statDropped;
    st.agentChannel = !agentMap.empty();
    int32_t left = (int32_t)(agentUnlockUntilMs - millis());
    st.agentUnlockedLeftMs = agentUnlocked && left > 0 ? left : 0;
    st.agentAsking = agentAsking;
    st.note = note;
}
