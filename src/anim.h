/**
 * Standby animations, uploaded from the paired computer over BLE (no reboot).
 *
 * The animation lives in the "spiffs" data partition (3.4 MB, otherwise unused)
 * as one UANM file, written by the OTA service (ota.cpp, begin command 03) while
 * the firmware keeps running. It plays straight to the panel - LVGL is paused
 * meanwhile - when the adapter has been idle for the standby delay, or on a
 * command from the computer; any activity (touch, button, BLE / USB change,
 * pop-up) stops it and brings the normal screen back.
 *
 * UANM v1, little-endian:
 *   header (32 B)  "UANM" | u8 version 1 | u8 scale (1 or 2) | u16 width | u16 height
 *                  | u16 frames | u16 frame_ms | u16 reserved | u16 background
 *                  | u16 reserved | u32 index offset | u32 file size | u32 reserved
 *   index          (frames + 1) x u32: offset of each frame, then the end of the last
 *   frame          u16 tokens: op = t >> 14, n = (t & 0x3FFF) + 1
 *                    op 0: skip n pixels (unchanged since the previous frame)
 *                    op 1: run - one pixel value follows, repeated n times
 *                    op 2: literal - n pixel values follow
 *                  Frame 0 is decoded onto the background colour.
 * Pixels are RGB565 as the panel takes them (high byte first on the wire), i.e.
 * the u16 stored is the byte-swapped RGB565 value. width x scale and height x
 * scale must be even and fit the 536 x 240 screen; the picture is centred on the
 * background colour. Encoder / uploader: tools/ble_anim.py.
 *
 * Commands from the computer (OTA control characteristic, ota.h):
 *   10 play now (until stopped or touched)    11 stop
 *   12 <mode u8> <delay_s u16> <brightness u8> <off_after_s u16> [<night u8>]   standby settings
 *        mode 0 = dim / off as before, 1 = play the animation when idle, 2 = the dashboard
 *        (clock, next meeting, the computer's load - ui.cpp), 3 = both in turns;
 *        brightness = by day, night = at night (see anim_brightness_now());
 *        off_after_s: screen off after this much standby animation (0 = never)
 *   13 info -> notification 10 <version u8> <frames u16> <width u16> <height u16>
 *        <scale u8> <frame_ms u16> <mode u8> <delay_s u16> <brightness u8> <off_after_s u16>
 *        <night u8> <clock valid u8> <hour u8> <minute u8>   (frames 0 = no animation stored)
 *   14 <year u16> <month u8> <day u8> <hour u8> <minute u8> <second u8>   set the clock
 *        (local time, into the RTC; the tools send it with every command)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Standby after the idle delay: nothing (dim / off), the animation, the
// dashboard (clock, next meeting, the computer's load - ui.cpp), or both in turns
enum : uint8_t { ANIM_MODE_OFF = 0, ANIM_MODE_STANDBY = 1, ANIM_MODE_DASHBOARD = 2, ANIM_MODE_BOTH = 3 };
enum AnimEvent : uint8_t { ANIM_EV_NONE, ANIM_EV_PLAY, ANIM_EV_STOP };

struct AnimSettings {
    uint8_t  mode;
    uint16_t delayS;      // idle time before the standby animation
    uint8_t  brightness;  // while it plays
    uint16_t offAfterS;   // screen off after this much standby animation, 0 = never
    uint8_t  nightBrightness;
    // The screen follows the keyboard: typing on a dark screen brings the standby
    // screen up, it stays while keys come, and goes dark kbdIdleS after the last
    // key. kbdIdleS 0 = the keyboard's own backlight timeout, read from its config
    // (bridge_keyboard_backlight_timeout_s(); 60 s while unknown). Commands 12 / 13
    // carry both after the night brightness: <kbd wake u8> <kbd idle s u16>; the
    // info reply adds <keyboard's timeout u16> (0 = unknown)
    bool     kbdWake;
    uint16_t kbdIdleS;
};

void anim_begin();                          // loads the stored animation and the settings
bool anim_available();                      // a valid animation is stored
const AnimSettings &anim_settings();
void anim_set_settings(const AnimSettings &s); // from the screen's settings menu (Arduino loop)
// Brightness for the standby animation / live spectrum at this time of day: night
// level until 6:00, up to the day level by 9:00, back down 18:00..22:00; the day
// level while the clock is not set (never set, or its backup cell ran flat)
uint8_t anim_brightness_now();
// The adapter's clock (RTC, set by the computer); false = not set yet
bool    anim_clock_now(uint8_t &hour, uint8_t &minute, uint8_t &second);

// Storage, from the OTA task: begin stops the player and erases, finish validates
uint32_t anim_storage_size();
bool     anim_storage_begin(uint32_t size);
bool     anim_storage_write(uint32_t offset, const uint8_t *data, size_t n);
bool     anim_storage_finish();
void     anim_storage_abort();

// Commands 10..13 from BLE (any task): queued, handled in anim_loop()
void anim_command(const uint8_t *cmd, size_t n);

// Arduino loop: commands, reload after an upload, frames while playing.
// Returns PLAY / STOP when the computer asked for it.
AnimEvent anim_loop();
void      anim_start();                     // takes over the panel (the caller sets the brightness)
void      anim_stop();                      // gives the panel back to LVGL (full redraw)
bool      anim_playing();
uint32_t  anim_played_ms();                 // since anim_start()
void      anim_redraw();                    // push the current frame again (the overlay changed)
