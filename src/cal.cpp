#include "cal.h"
#include <Arduino.h>
#include "log.h"
#include "ota.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static CalEvent     events[CAL_MAX_EVENTS];
static uint8_t      count;
static uint32_t     rxMs;
static bool         any;
static uint16_t     dismissed[8];     // ring of the last dismissed ids
static uint8_t      nDismissed, dismissedAt;

void cal_packet(const uint8_t *d, size_t n)
{
    if (n < 2 || d[0] != 0x15)
        return;
    CalEvent tmp[CAL_MAX_EVENTS];
    uint8_t  cnt = 0;
    size_t   p = 2;
    uint32_t now = millis();
    for (uint8_t i = 0; i < d[1] && cnt < CAL_MAX_EVENTS; i++) {
        if (p + 8 > n)
            break;
        CalEvent &e = tmp[cnt];
        e.id = d[p] | d[p + 1] << 8;
        int16_t startIn = d[p + 2] | d[p + 3] << 8;
        e.durMin = d[p + 4] | d[p + 5] << 8;
        e.flags = d[p + 6];
        uint8_t len = d[p + 7];
        p += 8;
        if (p + len > n)
            break;
        size_t tl = min<size_t>(len, CAL_TITLE_MAX);
        memcpy(e.title, d + p, tl);
        e.title[tl] = 0;
        p += len;
        e.startMs = now + (int32_t)startIn * 60000L;
        cnt++;
    }
    portENTER_CRITICAL(&mux);
    memcpy(events, tmp, sizeof(tmp));
    count = cnt;
    rxMs = now;
    any = true;
    portEXIT_CRITICAL(&mux);
}

bool cal_has_data()
{
    return any && millis() - rxMs < CAL_STALE_MS;
}

static bool isDismissed(uint16_t id)
{
    for (uint8_t i = 0; i < nDismissed; i++)
        if (dismissed[i] == id)
            return true;
    return false;
}

static int32_t sinceStartMs(const CalEvent &e, uint32_t now)
{
    return (int32_t)(now - e.startMs);
}

// First listed meeting that passes the filter
static bool pick(CalEvent &out, bool remind)
{
    if (!cal_has_data())
        return false;
    CalEvent tmp[CAL_MAX_EVENTS];
    uint8_t  cnt;
    portENTER_CRITICAL(&mux);
    memcpy(tmp, events, sizeof(tmp));
    cnt = count;
    portEXIT_CRITICAL(&mux);
    uint32_t now = millis();
    for (uint8_t i = 0; i < cnt; i++) {
        const CalEvent &e = tmp[i];
        int32_t since = sinceStartMs(e, now);
        if (isDismissed(e.id) || since > (int32_t)e.durMin * 60000L)
            continue;
        if (remind && (since < -CAL_REMIND_MIN * 60000L || since > CAL_LINGER_MIN * 60000L))
            continue;
        out = e;
        return true;
    }
    return false;
}

bool cal_next(CalEvent &e)
{
    return pick(e, false);
}

bool cal_reminder(CalEvent &e)
{
    return pick(e, true);
}

int32_t cal_minutes_to(const CalEvent &e)
{
    int32_t d = -sinceStartMs(e, millis());
    return d > 0 ? (d + 59999) / 60000 : -((-d) / 60000);
}

void cal_dismiss(uint16_t id)
{
    const uint8_t ring = sizeof(dismissed) / sizeof(dismissed[0]);
    if (!isDismissed(id)) {
        dismissed[dismissedAt] = id;
        dismissedAt = (dismissedAt + 1) % ring;
        nDismissed = min<uint8_t>(nDismissed + 1, ring);
    }
    uint8_t v[3] = {0x20, (uint8_t)id, (uint8_t)(id >> 8)};
    ota_notify(v, sizeof(v));
    logf("Calendar: meeting %u dismissed", id);
}
