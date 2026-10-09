#include "stats.h"
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "log.h"

static const char *NVS_NAMESPACE = "stats";

static Preferences   nvs;
static portMUX_TYPE  lock = portMUX_INITIALIZER_UNLOCKED; // counters are 64-bit, updated from the USB task
static StatsSnapshot totals, saved;
static uint32_t      savedAtMs = 0;

void stats_begin()
{
    nvs.begin(NVS_NAMESPACE, false);
    totals.keys = nvs.getULong64("keys", 0);
    totals.clicks = nvs.getULong64("clicks", 0);
    totals.travel = nvs.getDouble("travel", 0);
    saved = totals;
    logf("Stats: %llu keys, %llu clicks, %.0f m of mouse so far", (unsigned long long)totals.keys,
         (unsigned long long)totals.clicks, stats_meters(totals));
}

void stats_add(uint32_t keys, uint32_t clicks, float travel)
{
    portENTER_CRITICAL(&lock);
    totals.keys += keys;
    totals.clicks += clicks;
    totals.travel += travel;
    portEXIT_CRITICAL(&lock);
}

void stats_get(StatsSnapshot &st)
{
    portENTER_CRITICAL(&lock);
    st = totals;
    portEXIT_CRITICAL(&lock);
}

float stats_meters(const StatsSnapshot &st)
{
    return st.travel / MOUSE_CPI * 0.0254;
}

void stats_save()
{
    StatsSnapshot st;
    stats_get(st);
    savedAtMs = millis();
    if (st.keys == saved.keys && st.clicks == saved.clicks && st.travel == saved.travel)
        return;
    nvs.putULong64("keys", st.keys);
    nvs.putULong64("clicks", st.clicks);
    nvs.putDouble("travel", st.travel);
    saved = st;
}

void stats_loop(bool screenDark)
{
    static bool wasDark = false;
    if (screenDark && !wasDark)
        stats_save();
    wasDark = screenDark;
    if (millis() - savedAtMs >= STATS_SAVE_MS)
        stats_save();
}
