#include "live.h"
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include "display.h"

static const uint8_t  MIN_BANDS = 8, MAX_BANDS = 64;
static const uint16_t BASE_Y    = 172;           // bars stand on this row (even)
static const uint16_t BAR_MAX   = 166;           // tallest bar, px above BASE_Y
static const uint16_t CAP       = 2;             // peak cap height
static const uint16_t REFL_Y    = BASE_Y + 2;    // reflection starts here
static const float    REFL      = 0.4f;          // reflection height / bar height
static const uint16_t BAR_W_MAX = 32;
static const uint32_t FRAME_MS  = 16;            // redraw for the fall even without packets
static const float    FALL      = BAR_MAX * 2.2f; // px/s
static const float    HOLD_S    = 0.4f;          // peak cap hold before it drops
static const float    GRAVITY   = 600.0f;        // px/s^2 for the falling cap
static const uint16_t CLEAR_ROWS = 16;

// ---------------- Packets (BLE task) ----------------
static portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t           rxLevels[MAX_BANDS];
static uint8_t           rxBands, rxBright;
static bool              rxNew, fresh;
static volatile bool     ended = true;
static volatile uint32_t lastPacketMs;

void live_packet(const uint8_t *d, size_t n)
{
    if (n < 3 || d[0] != 'L' || d[1] != 'V') // also drops stray bytes of an aborted upload
        return;
    d += 2;
    n -= 2;
    if (d[0] == 0x00) {
        ended = true;
        return;
    }
    if (d[0] != 0x01 || n < 2u + MIN_BANDS)
        return;
    size_t bands = min<size_t>(n - 2, MAX_BANDS);
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    memcpy(rxLevels, d + 2, bands);
    rxBands = bands;
    rxBright = d[1];
    if (ended || now - lastPacketMs > LIVE_TIMEOUT_MS)
        fresh = true;
    ended = false;
    lastPacketMs = now;
    rxNew = true;
    portEXIT_CRITICAL(&mux);
}

bool live_streaming()
{
    return !ended && millis() - lastPacketMs < LIVE_TIMEOUT_MS;
}

bool live_take_fresh()
{
    portENTER_CRITICAL(&mux);
    bool f = fresh;
    fresh = false;
    portEXIT_CRITICAL(&mux);
    return f;
}

uint8_t live_brightness()
{
    return rxBright;
}

// ---------------- Drawing ----------------
struct Bar {
    uint16_t x, w;
    uint8_t  rgb[3];
    float    target, h, peak, peakVel, hold;
    int16_t  drawnH, drawnPeak, drawnRefl;   // what the panel shows
};
static Bar      bars[MAX_BANDS];
static uint8_t  nBands;
static bool     active;
static uint32_t lastDrawMs, lastStepMs;
static uint16_t rect[BAR_W_MAX * BASE_Y];     // one bar slice, rows are uniform

static uint16_t rgb565(float r, float g, float b)
{
    uint16_t v = ((uint16_t)r >> 3) << 11 | ((uint16_t)g >> 2) << 5 | (uint16_t)b >> 3;
    return v >> 8 | v << 8;                   // the panel takes the high byte first
}

static void hsv(float hDeg, uint8_t out[3])
{
    float h = fmodf(hDeg, 360.0f) / 60.0f, x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    switch ((int)h) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
    }
    out[0] = r * 255;
    out[1] = g * 255;
    out[2] = b * 255;
}

static void clearScreen()
{
    uint16_t w = display_width(), h = display_height();
    uint16_t *strip = (uint16_t *)ps_calloc((size_t)w * CLEAR_ROWS, 2);
    if (!strip)
        return;
    for (uint16_t y = 0; y < h; y += CLEAR_ROWS)
        display_push(0, y, w, min<uint16_t>(CLEAR_ROWS, h - y), strip);
    free(strip);
}

// Bars centred in equal slots, even x / width (the panel wants even windows);
// colours run pink (bass) -> violet -> cyan (treble)
static void layout(uint8_t n)
{
    nBands = n;
    float pitch = (float)display_width() / n;
    uint16_t w = min<uint16_t>(BAR_W_MAX, max<uint16_t>(2, (uint16_t)(pitch * 0.72f) & ~1));
    for (uint8_t b = 0; b < n; b++) {
        Bar &r = bars[b];
        memset(&r, 0, sizeof(r));
        r.w = w;
        r.x = (uint16_t)max(0.0f, (b + 0.5f) * pitch - w / 2.0f) & ~1;
        hsv(330.0f - 145.0f * b / max(1, n - 1), r.rgb);
    }
    clearScreen();
}

// Colour of row y of bar r for its current (drawn) state
static uint16_t pixel(const Bar &r, uint16_t y)
{
    if (y < BASE_Y) {
        int d = BASE_Y - 1 - y;               // 0 = the row on the base line
        if (d < r.drawnH) {
            float t = (float)d / (BAR_MAX - 1);
            float s = 0.42f + 0.58f * t;      // brighter towards the top
            float white = max(0.0f, (t - 0.78f) / 0.22f) * 0.55f;
            return rgb565((r.rgb[0] + (255 - r.rgb[0]) * white) * s, (r.rgb[1] + (255 - r.rgb[1]) * white) * s,
                          (r.rgb[2] + (255 - r.rgb[2]) * white) * s);
        }
        if (r.drawnPeak > 0 && d >= r.drawnPeak && d < r.drawnPeak + CAP)
            return rgb565(r.rgb[0] + (255 - r.rgb[0]) * 0.6f, r.rgb[1] + (255 - r.rgb[1]) * 0.6f,
                          r.rgb[2] + (255 - r.rgb[2]) * 0.6f);
        return 0;
    }
    int d = y - REFL_Y;
    if (d >= 0 && d < r.drawnRefl) {
        float s = 0.32f * powf(1.0f - (float)d / (display_height() - REFL_Y), 1.5f);
        return rgb565(r.rgb[0] * s, r.rgb[1] * s, r.rgb[2] * s);
    }
    return 0;
}

static void pushRows(const Bar &r, int y0, int y1)
{
    y0 = max(0, y0) & ~1;
    y1 = min<int>(display_height(), (y1 + 1) & ~1);
    while (y0 < y1) {
        int rows = min<int>(y1 - y0, BASE_Y);
        uint16_t *p = rect;
        for (int y = y0; y < y0 + rows; y++) {
            uint16_t c = pixel(r, y);
            for (uint16_t x = 0; x < r.w; x++)
                *p++ = c;
        }
        display_push(r.x, y0, r.w, rows, rect);
        y0 += rows;
    }
}

// Pushes only the rows that differ between the drawn and the new state
static void drawBar(Bar &r)
{
    int16_t h = (int16_t)r.h, peak = max<int16_t>((int16_t)r.peak, h);
    if (peak < 1)
        peak = 0;                             // no cap lying on the base line in silence
    int16_t refl = min<int16_t>((int16_t)(h * REFL), display_height() - REFL_Y);

    // up to three spans in "height above the base line": bar top, old cap, new cap
    int16_t span[3][2];
    int ns = 0;
    if (h != r.drawnH)
        span[ns][0] = min(h, r.drawnH), span[ns++][1] = max(h, r.drawnH);
    if (peak != r.drawnPeak) {
        if (r.drawnPeak)
            span[ns][0] = r.drawnPeak, span[ns++][1] = r.drawnPeak + CAP;
        if (peak)
            span[ns][0] = peak, span[ns++][1] = peak + CAP;
    }
    bool reflChanged = refl != r.drawnRefl;
    int16_t oldRefl = r.drawnRefl;
    r.drawnH = h;
    r.drawnPeak = peak;
    r.drawnRefl = refl;

    // merge overlapping / close spans (fewer, slightly larger windows)
    for (int i = 0; i < ns; i++)
        for (int j = i + 1; j < ns; j++)
            if (span[j][0] <= span[i][1] + 4 && span[i][0] <= span[j][1] + 4) {
                span[i][0] = min(span[i][0], span[j][0]);
                span[i][1] = max(span[i][1], span[j][1]);
                span[j][0] = span[--ns][0];
                span[j][1] = span[ns][1];
                j = i;                        // rescan against the grown span
            }
    for (int i = 0; i < ns; i++)
        pushRows(r, BASE_Y - span[i][1], BASE_Y - span[i][0]);
    if (reflChanged)
        pushRows(r, REFL_Y + min(refl, oldRefl), REFL_Y + max(refl, oldRefl));
}

static void step(Bar &r, float dt)
{
    r.h = max(r.target, r.h - FALL * dt);
    if (r.h >= r.peak) {
        r.peak = r.h;
        r.peakVel = 0;
        r.hold = HOLD_S;
    } else if (r.hold > 0) {
        r.hold -= dt;
    } else {
        r.peakVel += GRAVITY * dt;
        r.peak = max(r.h, r.peak - r.peakVel * dt);
    }
}

void live_start()
{
    if (active)
        return;
    nBands = 0;                               // layout (and clear) with the first frame
    clearScreen();
    lastDrawMs = lastStepMs = millis();
    active = true;
}

void live_stop()
{
    if (!active)
        return;
    active = false;
    lv_obj_invalidate(lv_scr_act());          // LVGL repaints everything we drew over
    lv_obj_invalidate(lv_layer_top());
}

bool live_active()
{
    return active;
}

void live_loop()
{
    if (!active)
        return;
    uint8_t levels[MAX_BANDS], n = 0;
    portENTER_CRITICAL(&mux);
    if (rxNew) {
        n = rxBands;
        memcpy(levels, rxLevels, n);
        rxNew = false;
    }
    portEXIT_CRITICAL(&mux);

    uint32_t now = millis();
    if (n) {
        if (n != nBands)
            layout(n);
        for (uint8_t b = 0; b < n; b++)
            bars[b].target = levels[b] * (float)BAR_MAX / 255.0f;
    } else if (!nBands || now - lastDrawMs < FRAME_MS) {
        return;
    }
    float dt = min((now - lastStepMs) / 1000.0f, 0.1f);
    lastStepMs = lastDrawMs = now;
    for (uint8_t b = 0; b < nBands; b++) {
        step(bars[b], dt);
        drawBar(bars[b]);
    }
}
