#include "display.h"
#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include "config.h"

static LilyGo_Class amoled;

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
    if (level != amoled.getBrightness())
        amoled.setBrightness(level);
}

uint8_t display_brightness()
{
    return amoled.getBrightness();
}

bool display_get_touch(int16_t *x, int16_t *y)
{
    return amoled.getPoint(x, y) > 0;
}
