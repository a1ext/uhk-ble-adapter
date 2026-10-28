/**
 * Overlay composited onto raw panel pushes: the meeting reminder card stays
 * visible over the standby animation and the live spectrum, which draw past
 * LVGL straight to the panel (display_push). The card is an LVGL object; a
 * snapshot of it (LV_IMG_CF_TRUE_COLOR_ALPHA: lv_color_t + alpha per pixel)
 * is handed here, and every push that meets its rectangle is blended through
 * a scratch copy, so the owners of the panel need not know about it.
 * Rectangles are even-aligned (the panel wants even windows).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef void (*overlay_push_fn)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels);

void overlay_set(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *argb); // 3 bytes per pixel
void overlay_clear();
bool overlay_active();
bool overlay_rect(uint16_t &x, uint16_t &y, uint16_t &w, uint16_t &h);
// A raw window on its way to the panel: rows outside the overlay go through as
// they are, rows inside are blended into a scratch copy first
void overlay_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels, overlay_push_fn raw);
// The overlay alone (over black): right after overlay_set, until whatever lies
// beneath repaints itself through overlay_push
void overlay_show(overlay_push_fn raw);
