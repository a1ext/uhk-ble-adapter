#include "ota.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_ota_ops.h>
#include <freertos/stream_buffer.h>
#include <mbedtls/sha256.h>
#include "ble_hid.h"
#include "config.h"
#include "log.h"

// Arduino core hook: do not auto-confirm a freshly updated image at boot,
// ota_loop() confirms it once the firmware has proven itself
extern "C" bool verifyRollbackLater()
{
    return true;
}

static const uint32_t OTA_CONFIRM_MS  = 5 * 60 * 1000UL; // confirm anyway after this uptime
static const uint32_t HOST_CONFIRM_MS = 3000;            // ... or a host stays connected this long
static const uint32_t DATA_TIMEOUT_MS = 15000;
static const size_t   STREAM_SIZE     = 16384;           // > the host window (12 KB)
static const uint32_t ACK_EVERY       = 4096;

enum : uint8_t {
    OTA_ST_RECEIVING = 0x01, OTA_ST_DONE = 0x02,
    OTA_ERR_BEGIN = 0x81, OTA_ERR_OVERFLOW = 0x82, OTA_ERR_WRITE = 0x83, OTA_ERR_IMAGE = 0x84,
    OTA_ERR_SHA = 0x85, OTA_ERR_SIZE = 0x86, OTA_ERR_ABORTED = 0x87, OTA_ERR_TIMEOUT = 0x88,
};
enum : uint8_t { CMD_BEGIN = 0x01, CMD_ABORT = 0x02 };

struct Req {
    uint8_t  cmd;
    uint32_t size;
    uint8_t  sha[32];
};

static BLECharacteristic *ctrlChr;
static StreamBufferHandle_t stream;
static QueueHandle_t reqQueue;
static volatile bool     active = false;     // between begin and end / abort
static volatile bool     overflow = false;
static volatile uint32_t received = 0, total = 0;
static volatile bool     restartPending = false;
static uint32_t          restartAtMs = 0;
static bool              pendingVerify = false;

static void notifyState(uint8_t state, uint32_t offset)
{
    uint8_t v[5] = {state, (uint8_t)offset, (uint8_t)(offset >> 8), (uint8_t)(offset >> 16),
                    (uint8_t)(offset >> 24)};
    ctrlChr->setValue(v, sizeof(v));
    ctrlChr->notify();
}

// ---------------- BLE callbacks (BTC task: queue only) ----------------
class CtrlCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override
    {
        if (!ble_hid_ready()) // only the active slot's authenticated host
            return;
        std::string v = c->getValue();
        Req r{};
        if (v.size() >= 37 && (uint8_t)v[0] == CMD_BEGIN) {
            r.cmd = CMD_BEGIN;
            memcpy(&r.size, v.data() + 1, 4); // little-endian, like the ESP32
            memcpy(r.sha, v.data() + 5, 32);
        } else if (v.size() >= 1 && (uint8_t)v[0] == CMD_ABORT) {
            r.cmd = CMD_ABORT;
        } else {
            return;
        }
        xQueueSend(reqQueue, &r, 0);
    }
};

class DataCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override
    {
        if (!active || !ble_hid_ready())
            return;
        std::string v = c->getValue();
        // The host keeps its window below the buffer size; no room = it did not
        if (xStreamBufferSend(stream, v.data(), v.size(), 0) != v.size())
            overflow = true;
    }
};

// ---------------- Flash writer task ----------------
static void otaTask(void *)
{
    static uint8_t buf[4096];
    esp_ota_handle_t handle = 0;
    const esp_partition_t *part = nullptr;
    mbedtls_sha256_context sha;
    uint8_t expected[32];
    uint32_t lastAck = 0, lastDataMs = 0;

    auto fail = [&](uint8_t err, const char *why) {
        esp_ota_abort(handle);
        mbedtls_sha256_free(&sha);
        active = false;
        logf("OTA: failed - %s", why);
        notifyState(err, received);
    };

    for (;;) {
        Req r;
        if (xQueueReceive(reqQueue, &r, active ? 0 : pdMS_TO_TICKS(100)) == pdTRUE) {
            if (r.cmd == CMD_ABORT) {
                if (active)
                    fail(OTA_ERR_ABORTED, "aborted");
                continue;
            }
            if (active)
                fail(OTA_ERR_ABORTED, "restarted by the host");
            part = esp_ota_get_next_update_partition(nullptr);
            if (!part || r.size == 0 || r.size > part->size) {
                logf("OTA: bad image size %lu", (unsigned long)r.size);
                notifyState(OTA_ERR_SIZE, 0);
                continue;
            }
            logf("OTA: %lu bytes into %s, erasing...", (unsigned long)r.size, part->label);
            xStreamBufferReset(stream);
            overflow = false;
            received = lastAck = 0;
            total = r.size;
            memcpy(expected, r.sha, sizeof(expected));
            esp_err_t e = esp_ota_begin(part, r.size, &handle); // erases the region (seconds)
            if (e != ESP_OK) {
                logf("OTA: begin failed: %s", esp_err_to_name(e));
                notifyState(OTA_ERR_BEGIN, 0);
                continue;
            }
            mbedtls_sha256_init(&sha);
            mbedtls_sha256_starts_ret(&sha, 0);
            active = true;
            lastDataMs = millis();
            notifyState(OTA_ST_RECEIVING, 0); // ready for data
        }
        if (!active)
            continue;
        if (overflow) {
            fail(OTA_ERR_OVERFLOW, "data overflow (host window too large)");
            continue;
        }

        size_t n = xStreamBufferReceive(stream, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n) {
            lastDataMs = millis();
            if (received + n > total) {
                fail(OTA_ERR_SIZE, "more data than announced");
                continue;
            }
            if (esp_ota_write(handle, buf, n) != ESP_OK) {
                fail(OTA_ERR_WRITE, "flash write");
                continue;
            }
            mbedtls_sha256_update_ret(&sha, buf, n);
            received += n;
            if (received - lastAck >= ACK_EVERY || received == total) {
                lastAck = received;
                notifyState(OTA_ST_RECEIVING, received);
            }
        } else if (millis() - lastDataMs > DATA_TIMEOUT_MS) {
            fail(OTA_ERR_TIMEOUT, "no data");
            continue;
        }

        if (received == total) {
            uint8_t got[32];
            mbedtls_sha256_finish_ret(&sha, got);
            mbedtls_sha256_free(&sha);
            active = false;
            if (memcmp(got, expected, sizeof(got))) {
                esp_ota_abort(handle);
                logf("OTA: failed - SHA-256 mismatch");
                notifyState(OTA_ERR_SHA, received);
                continue;
            }
            esp_err_t e = esp_ota_end(handle); // validates the image (header, checksum, hash)
            if (e == ESP_OK)
                e = esp_ota_set_boot_partition(part);
            if (e != ESP_OK) {
                logf("OTA: failed - image rejected: %s", esp_err_to_name(e));
                notifyState(OTA_ERR_IMAGE, received);
                continue;
            }
            logf("OTA: image OK, rebooting into %s", part->label);
            notifyState(OTA_ST_DONE, received);
            restartAtMs = millis() + 1500; // let the notification go out
            restartPending = true;
        }
    }
}

// ---------------- Public API ----------------
void ota_init()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        pendingVerify = true;
        logf("OTA: new firmware in %s - confirmed once a host reconnects", running->label);
    }
    stream = xStreamBufferCreate(STREAM_SIZE, 1);
    reqQueue = xQueueCreate(4, sizeof(Req));
    xTaskCreatePinnedToCore(otaTask, "ota", 6144, nullptr, 3, nullptr, 1);
}

void ota_create_service(BLEServer *server)
{
    BLEService *svc = server->createService(BLEUUID(OTA_SERVICE_UUID), 10, 0);
    // Passkey-paired link only, and the active slot's host (checked on write)
    ctrlChr = svc->createCharacteristic(BLEUUID(OTA_CTRL_UUID), BLECharacteristic::PROPERTY_WRITE
                                                                    | BLECharacteristic::PROPERTY_NOTIFY);
    ctrlChr->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
    BLE2902 *cccd = new BLE2902();
    cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
    ctrlChr->addDescriptor(cccd);
    ctrlChr->setCallbacks(new CtrlCallbacks());

    BLECharacteristic *data = svc->createCharacteristic(BLEUUID(OTA_DATA_UUID),
                                                        BLECharacteristic::PROPERTY_WRITE_NR);
    data->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
    data->setCallbacks(new DataCallbacks());
    svc->start();
}

void ota_loop(bool hostConnected)
{
    static bool abortSent = false;
    if (active && !hostConnected && !abortSent) { // host gone mid-transfer
        Req r{CMD_ABORT, 0, {}};
        xQueueSend(reqQueue, &r, 0);
        abortSent = true;
    }
    if (!active)
        abortSent = false;

    if (restartPending && (int32_t)(millis() - restartAtMs) >= 0)
        esp_restart();

    static uint32_t hostSinceMs = 0;
    if (!hostConnected)
        hostSinceMs = 0;
    else if (!hostSinceMs)
        hostSinceMs = millis();
    if (pendingVerify && ((hostSinceMs && millis() - hostSinceMs > HOST_CONFIRM_MS)
                          || millis() > OTA_CONFIRM_MS)) {
        esp_ota_mark_app_valid_cancel_rollback();
        pendingVerify = false;
        logf("OTA: new firmware confirmed");
    }
}

void ota_get_status(OtaStatus &st)
{
    st.active = active;
    st.received = received;
    st.total = total;
    st.pendingVerify = pendingVerify;
}
