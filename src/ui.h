/**
 * Touch UI (LVGL 8): the active slot's name as the title, the slot selector
 * (a little PCB: gold pads left / right of a bus, the active one neon),
 * keyboard / BLE status, typing statistics and the log tail. Pop-ups:
 * the pairing passkey and the UHK Agent "allow config access?" prompt.
 */
#pragma once
#include <stdint.h>

struct UiCallbacks {
    void (*onSlotRequest)(uint8_t slot); // user tapped a slot
    void (*onPair)();                    // user held "Pair" (start pairing mode)
    void (*onPairCancel)();              // user tapped the button during pairing
    void (*onAgentAllow)(bool allow);    // UHK Agent config access: allow / deny or lock
};

void ui_begin(const UiCallbacks &cb);
// Pairing button hold progress, 0..1000 (0 = not held: hidden)
void ui_set_hold_progress(uint16_t permille);
// bootNote: shown when nothing more important is pending (e.g. crash recovery)
void ui_update(const char *bootNote);
