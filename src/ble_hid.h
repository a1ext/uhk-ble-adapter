/**
 * BLE HID device (HID over GATT) with a report map supplied at runtime and
 * BLE_SLOTS host slots.
 *
 * Every slot is its own BLE identity: a static random address and the name
 * "<BLE_DEVICE_NAME> <n>". A host pairs with one slot and only reconnects to
 * that slot's address, so selecting a slot switches the keyboard to its host.
 *
 * Visibility: normally the adapter advertises connectable but NOT
 * discoverable, and only for a slot that has a host - the host reconnects,
 * nobody new sees it in a pairing list. Pairing mode (ble_hid_start_pairing)
 * drops the current host and makes the active slot discoverable for
 * PAIRING_WINDOW_MS; pairing a used slot replaces its host and moves the slot
 * to a fresh address (the old host cannot keep reconnecting). Pairing
 * requests outside pairing mode are refused.
 *
 * Security: pairing is LE Secure Connections with passkey entry - the
 * adapter shows a 6-digit code on the screen (BleHidStatus::passkey), the
 * host types it; Just Works and legacy pairing are refused, all HID / OTA
 * attributes need such an authenticated link. Bonds are not tied to our
 * address, so after encryption the host's identity (resolved private
 * address) must be the active slot's host - otherwise it is disconnected.
 * Reports flow only after that check (ble_hid_ready); a link that is not
 * authenticated in time is dropped.
 *
 * Optionally a second HID service carries the UHK Agent command channel: on
 * the host it is a separate HID device without report IDs, like the
 * keyboard's USB interface (macOS does not let apps open a HID device that
 * also contains a keyboard).
 *
 * Bluedroid callbacks run in the BTC task with a small stack, so they only
 * update state here; ble_hid_poll() (Arduino loop) logs, persists and
 * (re)starts advertising.
 */
#pragma once
#include <stdint.h>
#include <vector>
#include "config.h"
#include "hid_desc.h"

struct BleHidReportDef {
    HidReportType type;
    uint8_t  id;      // report ID in the BLE report map (internal ID for agent reports)
    uint16_t bytes;   // payload size (report ID excluded)
    bool     agent;   // belongs to the Agent HID service (report ID 0 there)
};

// Host wrote an output (e.g. keyboard LEDs) or feature report. Runs in the BTC task.
typedef void (*BleHidWriteCb)(HidReportType type, uint8_t id, const uint8_t *data, uint16_t len);

struct BleHidStatus {
    bool     started, enabled, connected, ready;
    uint16_t mtu;
    uint16_t interval;                 // connection interval, 1.25 ms units (0 = unknown)
    uint8_t  slot;                     // active host slot
    bool     slotUsed[BLE_SLOTS];      // a host is paired on the slot
    uint8_t  slotHost[BLE_SLOTS][6];   // its address
    uint32_t pairingLeftMs;            // > 0 while in pairing mode
    bool     passkeyShown;             // a host is pairing: show the passkey
    uint32_t passkey;                  // 6 digits, typed on the host
};

void ble_hid_init();                // loads the host slots from NVS
// agentMap: report descriptor of the Agent HID service, empty = no such service.
// pnpVid/Pid/Version: Device Information PnP ID (USB-IF vendor ID source)
void ble_hid_begin(const std::vector<uint8_t> &reportMap,
                   const std::vector<BleHidReportDef> &reports,
                   const std::vector<uint8_t> &agentMap, BleHidWriteCb onWrite,
                   uint16_t pnpVid, uint16_t pnpPid, uint16_t pnpVersion);
bool ble_hid_started();
void ble_hid_set_enabled(bool on);  // on: serve the active slot; off: disconnect, stop advertising
void ble_hid_poll();                // from the Arduino loop
bool ble_hid_ready();               // the active slot's host, authenticated: reports flow
bool ble_hid_can_send();            // ready and the controller has room for another notification
void ble_hid_send(uint8_t id, const uint8_t *data, uint16_t len);
void ble_hid_select_slot(uint8_t slot);
void ble_hid_start_pairing();       // active slot: discoverable, accepts a new device
void ble_hid_stop_pairing();
void ble_hid_forget_pairings();     // all slots
void ble_hid_get_status(BleHidStatus &st);
