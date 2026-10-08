/**
 * Touch UI (LVGL 8): iOS-style Cable / Bluetooth switch, a vertical host
 * slot strip with a neon bubble, keyboard / BLE status, counters and the log
 * tail. Slot and pairing controls are only shown in Bluetooth mode. Pop-ups:
 * the pairing passkey and the UHK Agent "allow config access?" prompt.
 */
#pragma once
#include <stdint.h>

struct UiCallbacks {
    void (*onModeRequest)(bool bt);      // user flipped the switch / tapped a mode label
    void (*onSlotRequest)(uint8_t slot); // user tapped a slot on the strip
    void (*onPair)();                    // user held "Pair" (start pairing mode)
    void (*onPairCancel)();              // user tapped the button during pairing
    void (*onAgentAllow)(bool allow);    // UHK Agent config access: allow / deny or lock
};

void ui_begin(const UiCallbacks &cb);
// Pairing button hold progress, 0..1000 (0 = not held: hidden)
void ui_set_hold_progress(uint16_t permille);
// btMode: the mode to show (the requested one while a switch is pending)
// bootNote: shown when nothing more important is pending (e.g. crash recovery)
void ui_update(bool btMode, const char *bootNote);
