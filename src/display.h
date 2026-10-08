/**
 * LilyGo T-Display-S3 AMOLED Plus: panel, touch and the LVGL display driver
 * (LilyGo-AMOLED-Series library, same setup as ../../Lilygo/ble-mouse).
 */
#pragma once
#include <stdint.h>

bool    display_begin();
void    display_set_brightness(uint8_t level); // 0 = off
uint8_t display_brightness();
bool    display_get_touch(int16_t *x, int16_t *y); // screen coordinates, false = no touch
