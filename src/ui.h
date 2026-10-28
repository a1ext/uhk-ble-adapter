/**
 * Touch UI (LVGL 8): the active slot's name as the title, the slot selector
 * (a little PCB: gold pads left / right of a bus, the active one neon),
 * keyboard / BLE status, typing statistics and the log tail. Pop-ups:
 * the pairing passkey, the UHK Agent "allow config access?" prompt and the
 * settings menu (host name with an on-screen keyboard, standby animation).
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
// Settings menu of the active slot: also opened by a tap on the active slot
void ui_open_menu();
void ui_close_menu();
bool ui_menu_open();
// Dashboard: the standby screen with the clock, the next meeting (cal.h) and
// the computer's load (sysmon.h), loaded instead of the main screen
void ui_show_dashboard(bool on);
bool ui_dashboard_shown();
// Meeting reminder card (cal.h), on the top layer of the LVGL screens. While
// the standby animation / live spectrum own the panel (panelTaken) a snapshot
// of it goes to the overlay compositor (overlay.h). Called every 100 ms;
// returns true when the overlay was set or cleared: the panel's owner repaints
bool ui_reminder_loop(bool panelTaken);
// A touch at x, y while LVGL gets no input: true = it hit the card's "X" (the
// meeting is dismissed, nothing else should happen)
bool ui_reminder_touch(int16_t x, int16_t y);
