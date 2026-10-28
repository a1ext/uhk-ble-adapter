#include "overlay.h"
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#define SCRATCH_ALLOC(n) ps_malloc(n)
#else
#define SCRATCH_ALLOC(n) malloc(n)
#endif

static struct {
    bool           active;
    uint16_t       x, y, w, h;
    const uint8_t *argb;
} ov;
static uint16_t *scratch;
static size_t    scratchPx;

static inline uint16_t swap16(uint16_t v)
{
    return v >> 8 | v << 8;
}

// The panel takes RGB565 high byte first; with LV_COLOR_16_SWAP the LVGL
// pixel is already in that order (the simulator renders without the swap)
static inline uint16_t lvToPanel(uint16_t v)
{
#if LV_COLOR_16_SWAP
    return v;
#else
    return swap16(v);
#endif
}

static inline uint16_t blend(uint16_t dst, uint16_t src, uint8_t a)
{
    if (a == 0)
        return dst;
    src = lvToPanel(src);
    if (a == 255)
        return src;
    uint16_t d = swap16(dst), s = swap16(src);
    uint32_t r = ((s >> 11) * a + (d >> 11) * (255 - a)) / 255;
    uint32_t g = ((s >> 5 & 63) * a + (d >> 5 & 63) * (255 - a)) / 255;
    uint32_t b = ((s & 31) * a + (d & 31) * (255 - a)) / 255;
    return swap16(r << 11 | g << 5 | b);
}

static bool ensureScratch(size_t px)
{
    if (px <= scratchPx)
        return true;
    free(scratch);
    scratch = (uint16_t *)SCRATCH_ALLOC(px * 2);
    scratchPx = scratch ? px : 0;
    return scratch != nullptr;
}

void overlay_set(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *argb)
{
    ov.x = x & ~1;
    ov.y = y & ~1;
    ov.w = w & ~1;
    ov.h = h & ~1;
    ov.argb = argb;
    ov.active = argb && ov.w && ov.h;
}

void overlay_clear()
{
    ov.active = false;
}

bool overlay_active()
{
    return ov.active;
}

bool overlay_rect(uint16_t &x, uint16_t &y, uint16_t &w, uint16_t &h)
{
    x = ov.x;
    y = ov.y;
    w = ov.w;
    h = ov.h;
    return ov.active;
}

// Blends the overlay into one row of a window (dst = the row, window x..x+w)
static void blendRow(uint16_t *dst, uint16_t x, uint16_t w, uint16_t y)
{
    uint16_t xa = x > ov.x ? x : ov.x, xb = x + w < ov.x + ov.w ? x + w : ov.x + ov.w;
    const uint8_t *o = ov.argb + ((size_t)(y - ov.y) * ov.w + (xa - ov.x)) * 3;
    for (uint16_t xx = xa; xx < xb; xx++, o += 3)
        dst[xx - x] = blend(dst[xx - x], o[0] | o[1] << 8, o[2]);
}

void overlay_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *px, overlay_push_fn raw)
{
    if (!ov.active || x >= ov.x + ov.w || x + w <= ov.x || y >= ov.y + ov.h || y + h <= ov.y) {
        raw(x, y, w, h, px);
        return;
    }
    uint16_t y0 = y > ov.y ? y : ov.y, y1 = y + h < ov.y + ov.h ? y + h : ov.y + ov.h;
    if (y0 > y)
        raw(x, y, w, y0 - y, px);
    uint16_t *inside = px + (size_t)(y0 - y) * w;
    if (ensureScratch((size_t)w * (y1 - y0))) {
        for (uint16_t yy = y0; yy < y1; yy++) {
            uint16_t *row = scratch + (size_t)(yy - y0) * w;
            memcpy(row, px + (size_t)(yy - y) * w, 2 * w);
            blendRow(row, x, w, yy);
        }
        inside = scratch;
    }
    raw(x, y0, w, y1 - y0, inside);
    if (y1 < y + h)
        raw(x, y1, w, y + h - y1, px + (size_t)(y1 - y) * w);
}

void overlay_show(overlay_push_fn raw)
{
    if (!ov.active || !ensureScratch((size_t)ov.w * ov.h))
        return;
    memset(scratch, 0, (size_t)ov.w * ov.h * 2);
    for (uint16_t yy = 0; yy < ov.h; yy++)
        blendRow(scratch + (size_t)yy * ov.w, ov.x, ov.w, ov.y + yy);
    raw(ov.x, ov.y, ov.w, ov.h, scratch);
}
