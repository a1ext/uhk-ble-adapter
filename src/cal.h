/**
 * Calendar: the next meetings, sent by the paired computer, and the reminder
 * shown before one starts.
 *
 * Command on the OTA control characteristic (ota.h; the tools send it every
 * minute and when the list changes):
 *   15 <n u8> then n x { <id u16> <start_in_min i16> <duration_min u16> <flags u8>
 *                        <title length u8> <title, ASCII> }
 *   Times are relative to "now" - the adapter counts them with millis(), so it
 *   needs no date or time zone. flags: 1 confirmed (accepted), 2 tentative.
 *   The list is forgotten CAL_STALE_MS after the last packet (the computer is
 *   gone, meetings may have been cancelled meanwhile).
 * Notification from the adapter:
 *   20 <id u16>   the reminder was dismissed on the screen: do not send this
 *                 meeting again (today)
 *
 * The reminder appears CAL_REMIND_MIN before the start (a dark screen wakes up
 * for it) and stays until CAL_LINGER_MIN after it; "X" dismisses it.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

static const uint8_t  CAL_MAX_EVENTS = 3;
static const uint8_t  CAL_TITLE_MAX  = 24;
static const int32_t  CAL_REMIND_MIN = 15;
static const int32_t  CAL_LINGER_MIN = 5;
static const uint32_t CAL_STALE_MS   = 2 * 60 * 60 * 1000UL;

enum : uint8_t { CAL_CONFIRMED = 1, CAL_TENTATIVE = 2 };

struct CalEvent {
    uint16_t id;
    uint32_t startMs;    // millis() of the start (may lie in the past)
    uint16_t durMin;
    uint8_t  flags;
    char     title[CAL_TITLE_MAX + 1];
};

void    cal_packet(const uint8_t *data, size_t n);  // BLE task
bool    cal_has_data();                             // a list arrived and is not stale
bool    cal_next(CalEvent &e);                      // the next meeting that is not over or dismissed
bool    cal_reminder(CalEvent &e);                  // the one to remind of right now
int32_t cal_minutes_to(const CalEvent &e);          // rounded up; <= 0 once it started
void    cal_dismiss(uint16_t id);                   // from the screen (Arduino loop)
