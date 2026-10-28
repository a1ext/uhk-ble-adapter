#include "anim.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <math.h>
#include <lvgl.h>
#include "display.h"
#include "log.h"
#include "ota.h"

static const uint32_t HEADER_SIZE = 32;
static const uint16_t MAX_FRAMES  = 4096;
static const uint16_t FILL_ROWS   = 16;  // background fill strip

struct __attribute__((packed)) Header {
    char     magic[4];
    uint8_t  version, scale;
    uint16_t width, height, frames, frameMs, reserved1, background, reserved2;
    uint32_t indexOffset, fileSize, reserved3;
};
static_assert(sizeof(Header) == HEADER_SIZE, "UANM header");

struct Cmd {
    uint8_t len;
    uint8_t data[15];
};

static const esp_partition_t *part;
static QueueHandle_t cmdQueue;
static volatile bool busy;          // an upload owns the partition
static volatile bool reloadPending;

static bool      loaded;
static Header    hdr;
static uint32_t *frameIndex;        // frames + 1 offsets
static uint8_t  *packed;            // one compressed frame
static uint16_t *frame;             // decoded, width x height
static uint16_t *scaled;            // scale 2: (2 width) x (2 height)
static uint16_t  outW, outH, outX, outY;

static bool     playing;
static uint16_t current;
static uint32_t nextFrameMs, startedMs;

static AnimSettings settings = {ANIM_MODE_STANDBY, 60, 180, 30 * 60, 30};

// Local time of day from the RTC, read once and then counted with millis()
static bool     clockValid;
static uint32_t clockBaseS, clockBaseMs;

// ---------------- File checks / loading ----------------
// Header and index sanity; fills the largest compressed frame size
static bool checkFile(Header &h, uint32_t *index, uint32_t &maxFrame)
{
    if (!part || esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK)
        return false;
    uint32_t w = h.width * h.scale, hh = h.height * h.scale;
    if (memcmp(h.magic, "UANM", 4) || h.version != 1 || (h.scale != 1 && h.scale != 2) || !h.width
        || !h.height || w > display_width() || hh > display_height() || (w & 1) || (hh & 1)
        || !h.frames || h.frames > MAX_FRAMES || h.frameMs < 10 || h.fileSize > part->size
        || h.indexOffset < HEADER_SIZE || h.indexOffset + 4UL * (h.frames + 1) > h.fileSize)
        return false;
    uint32_t prev = h.indexOffset + 4UL * (h.frames + 1);
    maxFrame = 0;
    for (uint32_t i = 0; i <= h.frames; i++) {
        uint32_t off;
        if (esp_partition_read(part, h.indexOffset + 4 * i, &off, 4) != ESP_OK || off < prev
            || off > h.fileSize)
            return false;
        if (i)
            maxFrame = max(maxFrame, off - prev);
        if (index)
            index[i] = off;
        prev = off;
    }
    return prev == h.fileSize && maxFrame > 0;
}

static void unload()
{
    loaded = false;
    free(frameIndex);
    free(packed);
    free(frame);
    free(scaled);
    frameIndex = nullptr;
    packed = nullptr;
    frame = nullptr;
    scaled = nullptr;
}

static bool load()
{
    unload();
    Header h;
    uint32_t maxFrame;
    if (!checkFile(h, nullptr, maxFrame))
        return false;
    frameIndex = (uint32_t *)ps_malloc(4UL * (h.frames + 1));
    packed = (uint8_t *)ps_malloc(maxFrame);
    frame = (uint16_t *)ps_malloc(2UL * h.width * h.height);
    outW = h.width * h.scale;
    outH = h.height * h.scale;
    scaled = h.scale == 2 ? (uint16_t *)ps_malloc(2UL * outW * outH) : nullptr;
    if (!frameIndex || !packed || !frame || (h.scale == 2 && !scaled) || !checkFile(h, frameIndex, maxFrame)) {
        unload();
        logf("Anim: out of memory");
        return false;
    }
    hdr = h;
    outX = ((display_width() - outW) / 2) & ~1;  // the panel wants even window corners
    outY = ((display_height() - outH) / 2) & ~1;
    loaded = true;
    logf("Anim: %u frames %ux%u x%u, %u ms, %lu KB", h.frames, h.width, h.height, h.scale, h.frameMs,
         (unsigned long)(h.fileSize / 1024));
    return true;
}

// ---------------- Playback ----------------
static bool decodeFrame(uint16_t i)
{
    uint32_t off = frameIndex[i], size = frameIndex[i + 1] - off;
    if (esp_partition_read(part, off, packed, size) != ESP_OK)
        return false;
    size_t pixels = (size_t)hdr.width * hdr.height, p = 0;
    if (i == 0)
        for (size_t k = 0; k < pixels; k++)
            frame[k] = hdr.background;
    const uint8_t *c = packed, *end = packed + size;
    while (c + 2 <= end) {
        uint16_t t = c[0] | c[1] << 8;
        c += 2;
        size_t n = (t & 0x3FFF) + 1;
        if (p + n > pixels)
            return false;
        switch (t >> 14) {
        case 0: // skip
            p += n;
            break;
        case 1: { // run
            if (c + 2 > end)
                return false;
            uint16_t v = c[0] | c[1] << 8;
            c += 2;
            while (n--)
                frame[p++] = v;
            break;
        }
        case 2: // literal
            if (c + 2 * n > end)
                return false;
            memcpy(&frame[p], c, 2 * n);
            c += 2 * n;
            p += n;
            break;
        default:
            return false;
        }
    }
    return true;
}

static void pushFrame()
{
    if (hdr.scale == 1) {
        display_push(outX, outY, outW, outH, frame);
        return;
    }
    for (uint16_t y = 0; y < hdr.height; y++) {
        const uint16_t *s = frame + (size_t)y * hdr.width;
        uint16_t *d = scaled + (size_t)2 * y * outW;
        for (uint16_t x = 0; x < hdr.width; x++)
            d[2 * x] = d[2 * x + 1] = s[x];
        memcpy(d + outW, d, 2UL * outW);
    }
    display_push(outX, outY, outW, outH, scaled);
}

static void fillBackground()
{
    uint16_t w = display_width(), h = display_height();
    uint16_t *strip = (uint16_t *)ps_malloc(2UL * w * FILL_ROWS);
    if (!strip)
        return;
    for (size_t k = 0; k < (size_t)w * FILL_ROWS; k++)
        strip[k] = hdr.background;
    for (uint16_t y = 0; y < h; y += FILL_ROWS)
        display_push(0, y, w, min<uint16_t>(FILL_ROWS, h - y), strip);
    free(strip);
}

void anim_start()
{
    if (!loaded || busy || playing)
        return;
    fillBackground();
    current = 0;
    nextFrameMs = startedMs = millis();
    playing = true;
}

void anim_stop()
{
    if (!playing)
        return;
    playing = false;
    lv_obj_invalidate(lv_scr_act()); // LVGL repaints everything we drew over
    lv_obj_invalidate(lv_layer_top());
}

bool anim_playing()
{
    return playing;
}

uint32_t anim_played_ms()
{
    return playing ? millis() - startedMs : 0;
}

// ---------------- Clock / brightness by the time of day ----------------
static void readClock()
{
    uint8_t h, m, s;
    clockValid = display_rtc_read(h, m, s);
    clockBaseS = h * 3600UL + m * 60 + s;
    clockBaseMs = millis();
}

static uint32_t secondsOfDay()
{
    return (clockBaseS + (millis() - clockBaseMs) / 1000) % 86400;
}

static float smoothstep(float a, float b, float x)
{
    float t = constrain((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

uint8_t anim_brightness_now()
{
    if (!clockValid)
        return settings.brightness;
    float h = secondsOfDay() / 3600.0f;
    float day = smoothstep(6, 9, h) * (1 - smoothstep(18, 22, h));
    // interpolate in perceived brightness (~ level^(1/2.2)), like the screen fades
    float pn = powf(settings.nightBrightness / 255.0f, 1 / 2.2f), pd = powf(settings.brightness / 255.0f, 1 / 2.2f);
    return max<uint8_t>(1, lroundf(255.0f * powf(pn + (pd - pn) * day, 2.2f)));
}

bool anim_clock_now(uint8_t &hour, uint8_t &minute, uint8_t &second)
{
    uint32_t sod = secondsOfDay();
    hour = sod / 3600;
    minute = sod / 60 % 60;
    second = sod % 60;
    return clockValid;
}

void anim_redraw()
{
    if (playing)
        pushFrame();
}

// ---------------- Settings / commands ----------------
static void saveSettings()
{
    Preferences nvs;
    nvs.begin("anim", false);
    nvs.putUChar("mode", settings.mode);
    nvs.putUShort("delay", settings.delayS);
    nvs.putUChar("bright", settings.brightness);
    nvs.putUShort("off", settings.offAfterS);
    nvs.putUChar("night", settings.nightBrightness);
    nvs.end();
}

static void logSettings()
{
    static const char *const modes[] = {"off", "animation", "dashboard", "dashboard + animation"};
    logf("Anim: standby %s after %u s, brightness %u / night %u, off after %u s",
         modes[min<uint8_t>(settings.mode, ANIM_MODE_BOTH)], settings.delayS, settings.brightness,
         settings.nightBrightness, settings.offAfterS);
}

static void sendInfo()
{
    uint16_t frames = loaded ? hdr.frames : 0;
    uint32_t sod = secondsOfDay();
    uint8_t v[21] = {0x10, 1, (uint8_t)frames, (uint8_t)(frames >> 8), (uint8_t)hdr.width,
                     (uint8_t)(hdr.width >> 8), (uint8_t)hdr.height, (uint8_t)(hdr.height >> 8), hdr.scale,
                     (uint8_t)hdr.frameMs, (uint8_t)(hdr.frameMs >> 8), settings.mode,
                     (uint8_t)settings.delayS, (uint8_t)(settings.delayS >> 8), settings.brightness,
                     (uint8_t)settings.offAfterS, (uint8_t)(settings.offAfterS >> 8), settings.nightBrightness,
                     clockValid, (uint8_t)(sod / 3600), (uint8_t)(sod / 60 % 60)};
    ota_notify(v, sizeof(v));
}

void anim_command(const uint8_t *cmd, size_t n)
{
    Cmd c{};
    c.len = min<size_t>(n, sizeof(c.data));
    memcpy(c.data, cmd, c.len);
    xQueueSend(cmdQueue, &c, 0);
}

static AnimEvent handle(const Cmd &c)
{
    switch (c.data[0]) {
    case 0x10:
        if (loaded)
            return ANIM_EV_PLAY;
        logf("Anim: nothing to play");
        break;
    case 0x11:
        return ANIM_EV_STOP;
    case 0x12:
        if (c.len >= 7) {
            settings.mode = min<uint8_t>(c.data[1], ANIM_MODE_BOTH);
            settings.delayS = max<uint16_t>(5, c.data[2] | c.data[3] << 8);
            settings.brightness = max<uint8_t>(1, c.data[4]);
            settings.offAfterS = c.data[5] | c.data[6] << 8;
            if (c.len >= 8)
                settings.nightBrightness = max<uint8_t>(1, c.data[7]);
            saveSettings();
            logSettings();
        }
        break;
    case 0x13:
        sendInfo();
        break;
    case 0x14:
        if (c.len >= 8 && c.data[3] >= 1 && c.data[3] <= 12 && c.data[4] >= 1 && c.data[4] <= 31
            && c.data[5] < 24 && c.data[6] < 60 && c.data[7] < 60) {
            bool was = clockValid;
            display_rtc_write(c.data[1] | c.data[2] << 8, c.data[3], c.data[4], c.data[5], c.data[6], c.data[7]);
            readClock();
            if (!was && clockValid)
                logf("Clock set: %02u:%02u", c.data[5], c.data[6]);
        }
        break;
    }
    return ANIM_EV_NONE;
}

AnimEvent anim_loop()
{
    if (busy) {          // an upload is erasing / writing the partition
        anim_stop();
        loaded = false;
    } else if (reloadPending) {
        reloadPending = false;
        load();
    }

    AnimEvent ev = ANIM_EV_NONE;
    Cmd c;
    if (xQueueReceive(cmdQueue, &c, 0) == pdTRUE && c.len)
        ev = handle(c);

    if (playing && (int32_t)(millis() - nextFrameMs) >= 0) {
        if (!decodeFrame(current)) {
            logf("Anim: frame %u is damaged - stopped", current);
            anim_stop();
            unload();
            return ANIM_EV_STOP;
        }
        pushFrame();
        current = current + 1 < hdr.frames ? current + 1 : 0;
        nextFrameMs += hdr.frameMs;
        if ((int32_t)(millis() - nextFrameMs) > 200) // fell behind: do not try to catch up
            nextFrameMs = millis();
    }
    return ev;
}

// ---------------- Storage (OTA task) ----------------
uint32_t anim_storage_size()
{
    return part ? part->size : 0;
}

bool anim_storage_begin(uint32_t size)
{
    if (!part || !size || size > part->size)
        return false;
    busy = true;
    vTaskDelay(pdMS_TO_TICKS(100)); // the loop stops the player before the data goes away
    return esp_partition_erase_range(part, 0, (size + 4095) & ~4095UL) == ESP_OK;
}

bool anim_storage_write(uint32_t offset, const uint8_t *data, size_t n)
{
    return esp_partition_write(part, offset, data, n) == ESP_OK;
}

bool anim_storage_finish()
{
    Header h;
    uint32_t maxFrame;
    bool ok = checkFile(h, nullptr, maxFrame);
    busy = false;
    reloadPending = true;
    return ok;
}

void anim_storage_abort()
{
    busy = false;
    reloadPending = true; // the partition holds a partial file now: nothing to play
}

// ---------------- Init ----------------
void anim_begin()
{
    cmdQueue = xQueueCreate(4, sizeof(Cmd));
    Preferences nvs;
    nvs.begin("anim", true);
    settings.mode = nvs.getUChar("mode", settings.mode);
    settings.delayS = nvs.getUShort("delay", settings.delayS);
    settings.brightness = nvs.getUChar("bright", settings.brightness);
    settings.offAfterS = nvs.getUShort("off", settings.offAfterS);
    settings.nightBrightness = nvs.getUChar("night", settings.nightBrightness);
    nvs.end();
    readClock();
    logf(clockValid ? "Clock: %02lu:%02lu (RTC)" : "Clock: not set - the computer sets it",
         (unsigned long)(clockBaseS / 3600), (unsigned long)(clockBaseS / 60 % 60));
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) {
        logf("Anim: no data partition");
        return;
    }
    load();
}

bool anim_available()
{
    return loaded;
}

const AnimSettings &anim_settings()
{
    return settings;
}

void anim_set_settings(const AnimSettings &s)
{
    if (s.mode == settings.mode && s.delayS == settings.delayS && s.brightness == settings.brightness
        && s.offAfterS == settings.offAfterS && s.nightBrightness == settings.nightBrightness)
        return;
    settings = s;
    settings.delayS = max<uint16_t>(5, settings.delayS);
    settings.brightness = max<uint8_t>(1, settings.brightness);
    settings.nightBrightness = max<uint8_t>(1, settings.nightBrightness);
    saveSettings();
    logSettings();
}
