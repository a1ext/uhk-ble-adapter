/**
 * USB -> BLE HID bridge.
 *
 * The BLE report map mirrors the keyboard's own USB report descriptors, so
 * whatever the keyboard firmware declares - NKRO bitfield, mouse with
 * high-resolution scrolling, consumer / system controls - is forwarded
 * unchanged. Of the vendor-defined interfaces only the UHK Agent command
 * channel is kept (BLE_AGENT_CHANNEL): read-only status commands pass,
 * reading / writing the user config, macros etc. need "Allow" on the screen
 * (bridge_agent_allow), firmware update commands are always refused. The descriptors are cached in NVS, so after the first run BLE
 * advertises right after boot, before the keyboard is enumerated.
 *
 * Input reports go through a FIFO: keyboard reports are never merged (fast
 * macros must keep every press / release), consecutive mouse reports with
 * the same button state are merged by summing the relative axes.
 */
#pragma once
#include <stdint.h>

struct BridgeStatus {
    bool     usbConnected;     // keyboard enumerated and polled
    char     usbProduct[32];
    bool     mapValid;         // BLE report map known
    uint32_t forwarded, merged, dropped;
    bool     agentChannel;     // UHK Agent channel served over BLE
    bool     agentAsking;      // Agent was refused config access: ask on the screen
    uint32_t agentUnlockedLeftMs; // > 0 while Agent config access is allowed
    const char *note;          // status message for the screen, may be nullptr
};

void bridge_begin();                // loads the cached HID map and the BLE host slots
void bridge_start();                // starts the USB host and (with a known map) BLE
void bridge_loop();                 // Arduino loop: deferred BLE start, restarts
void bridge_agent_allow(bool allow); // screen: allow / lock Agent config access
void bridge_get_status(BridgeStatus &st);
