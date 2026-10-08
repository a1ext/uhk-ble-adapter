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
