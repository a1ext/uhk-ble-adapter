/**
 * Typing statistics: key presses, mouse clicks and pointer travel, counted
 * from the keyboard's USB reports (bridge.cpp) and kept in NVS.
 *
 * The adapter is always powered, so the totals are written rarely: every
 * STATS_SAVE_MS, when the screen goes dark and before a deliberate restart.
 * A crash or power cut loses at most the time since the last write.
 */
#pragma once
#include <stdint.h>

struct StatsSnapshot {
    uint64_t keys, clicks;
    double   travel;      // pointer travel in mouse counts
};

void  stats_begin();                                    // loads the totals
void  stats_add(uint32_t keys, uint32_t clicks, float travel); // any task
void  stats_loop(bool screenDark);                      // Arduino loop: periodic / on-dark save
void  stats_save();                                     // writes now if anything changed
void  stats_get(StatsSnapshot &st);
float stats_meters(const StatsSnapshot &st);            // travel at MOUSE_CPI
