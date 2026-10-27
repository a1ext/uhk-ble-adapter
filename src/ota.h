/**
 * Firmware update over BLE (A/B: the inactive app partition is written while
 * the current firmware keeps running).
 *
 * GATT service (passkey-paired link of the active slot's host only):
 *   control  OTA_CTRL_UUID  write + notify
 *     host -> adapter  01 <size u32 LE> <sha256 32 B>   begin a firmware image
 *                      03 <size u32 LE> <sha256 32 B>   begin a standby animation (UANM,
 *                                                       anim.h): data partition, no reboot
 *                      02                               abort
 *                      10..13                           animation control (anim.h)
 *     adapter -> host  <state u8> <offset u32 LE>
 *                      state 1 = ready / receiving (offset = bytes written),
 *                      2 = image OK, rebooting, 3 = animation stored (plays from now on),
 *                      >= 0x80 = error (see ota.cpp); 0x10 = animation info (anim.h)
 *   data     OTA_DATA_UUID  write without response: the image bytes in order.
 *            Flow control: keep at most OTA_WINDOW bytes beyond the last
 *            acknowledged offset. Outside a transfer: live spectrum packets (live.h).
 *   Do not add characteristics: Windows caches the table of a bonded device and
 *   fails uncached discovery after a change (only re-pairing fixes it).
 *
 * The new image boots in pending-verify state and is confirmed once a BLE
 * host reconnects (or after OTA_CONFIRM_MS); a crash / reset before that rolls
 * the bootloader back to the previous firmware. Host side: tools/ble_ota.py.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define OTA_SERVICE_UUID "6a1e0100-2b5e-4f8a-9c3d-75ab8e4f0a01"
#define OTA_CTRL_UUID    "6a1e0101-2b5e-4f8a-9c3d-75ab8e4f0a01"
#define OTA_DATA_UUID    "6a1e0102-2b5e-4f8a-9c3d-75ab8e4f0a01"
class BLEServer;

struct OtaStatus {
    bool     active;     // receiving / verifying an image
    bool     animation;  // ... and it is a standby animation, not firmware
    uint32_t received, total;
    bool     pendingVerify; // running a freshly updated, not yet confirmed image
};

void ota_init();                       // before BLE: rollback bookkeeping, task
void ota_create_service(BLEServer *server);
// From the Arduino loop. hostConnected: the active slot's host is connected
// and authenticated (confirms a freshly updated image)
void ota_loop(bool hostConnected);
void ota_get_status(OtaStatus &st);
void ota_notify(const uint8_t *data, size_t n); // raw notification on the control characteristic
