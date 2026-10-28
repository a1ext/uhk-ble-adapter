/**
 * LilyGo T-Display-S3 AMOLED Plus: panel, touch and the LVGL display driver
 * (LilyGo-AMOLED-Series library, same setup as ../../Lilygo/ble-mouse).
 */
#pragma once
#include <stdint.h>

bool    display_begin();
void    display_set_brightness(uint8_t level); // 0 = off; immediate, cancels a fade
void    display_fade_to(uint8_t level, uint32_t ms); // smooth, slowing down towards the end
void    display_loop();                         // from the Arduino loop: runs the fade
uint8_t display_brightness();
bool    display_get_touch(int16_t *x, int16_t *y); // screen coordinates, false = no touch
uint16_t display_width();
uint16_t display_height();
// Raw pixels straight to the panel, bypassing LVGL (standby animation). RGB565
// as LVGL keeps it (byte-swapped); x, y, w, h even
void    display_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels);
// The overlay (overlay.h) alone, over black: after it was set while something
// drew raw pixels (the owner of the panel repaints the rest through display_push)
void    display_overlay_show();
void    display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h); // black, through the overlay

// The board's PCF85063 RTC (local time, set from the computer), backed by a
// rechargeable MS412FE cell (U8, charged from 3V3 via D9). False when its
// oscillator stopped since the time was set (the cell ran flat)
bool    display_rtc_read(uint8_t &hour, uint8_t &minute, uint8_t &second);
bool    display_rtc_read_date(uint16_t &year, uint8_t &month, uint8_t &day); // false = not set
void    display_rtc_write(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                          uint8_t second);
