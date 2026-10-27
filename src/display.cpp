#include "display.h"
#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include <Wire.h>
#include <math.h>
#include "config.h"

static LilyGo_Class amoled;

// Brightness fade, interpolated in perceived brightness (~ level^(1/2.2)),
// otherwise the low end of a fade looks like a sudden drop
static struct {
    bool     active;
    float    from, to;    // perceived levels 0..1
    uint8_t  target;
    uint32_t startMs, durationMs;
} fade;

static float perceived(uint8_t level)
{
    return powf(level / 255.0f, 1 / 2.2f);
}

static void writeBrightness(uint8_t level)
{
    if (level != amoled.getBrightness())
        amoled.setBrightness(level);
}

bool display_begin()
{
    // Auto-detects the board variant (Plus = 1.91" SPI + PMU), powers the
    // panel, initializes the RM67162 display and CST816T touch controller
    if (!amoled.begin())
        return false;
    amoled.setRotation(DISPLAY_ROTATION);
    beginLvglHelper(amoled);
    amoled.setBrightness(DISPLAY_BRIGHTNESS);
    return true;
}

void display_set_brightness(uint8_t level)
{
    fade.active = false;
    writeBrightness(level);
}

void display_fade_to(uint8_t level, uint32_t ms)
{
    if (fade.active ? fade.target == level : amoled.getBrightness() == level)
        return;
    fade.from = perceived(amoled.getBrightness());
    fade.to = perceived(level);
    fade.target = level;
    fade.startMs = millis();
    fade.durationMs = max<uint32_t>(ms, 1);
    fade.active = true;
}

void display_loop()
{
    if (!fade.active)
        return;
    uint32_t t = millis() - fade.startMs;
    if (t >= fade.durationMs) {
        fade.active = false;
        writeBrightness(fade.target);
        return;
    }
    // Cubic ease-out: most of the change early, slowing down towards the end
    float rest = 1.0f - (float)t / fade.durationMs;
    float p = fade.to + (fade.from - fade.to) * rest * rest * rest;
    writeBrightness((uint8_t)lroundf(255.0f * powf(p, 2.2f)));
}

uint8_t display_brightness()
{
    return amoled.getBrightness();
}

bool display_get_touch(int16_t *x, int16_t *y)
{
    return amoled.getPoint(x, y) > 0;
}

uint16_t display_width()
{
    return amoled.width();
}

uint16_t display_height()
{
    return amoled.height();
}

void display_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels)
{
    amoled.pushColors(x, y, w, h, pixels);
}

static const uint8_t PCF85063_ADDR = 0x51, PCF85063_SECONDS = 0x04, PCF85063_OS = 0x80;

bool display_rtc_read(uint8_t &hour, uint8_t &minute, uint8_t &second)
{
    if (!amoled.hasRTC())
        return false;
    // OS flag: the oscillator stopped (power loss) since the time was last set
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(PCF85063_SECONDS);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(PCF85063_ADDR, (uint8_t)1) != 1
        || (Wire.read() & PCF85063_OS))
        return false;
    RTC_DateTime t = amoled.getDateTime();
    if (t.hour > 23 || t.minute > 59 || t.second > 59)
        return false;
    hour = t.hour;
    minute = t.minute;
    second = t.second;
    return true;
}

void display_rtc_write(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second)
{
    if (amoled.hasRTC())
        amoled.setDateTime(year, month, day, hour, minute, second); // also clears the OS flag
}
