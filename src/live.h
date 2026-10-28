/**
 * Live spectrum from the paired computer: it captures its audio, runs the FFT
 * and streams band levels; the adapter draws neon bars with a reflection and
 * falling peak caps (the ballistics run here, the computer sends raw levels).
 *
 * Packets on the OTA data characteristic while no transfer runs (ota.h; write
 * without response, passkey-paired link of the active slot's host only):
 *   "LV" 01 <brightness u8> <level u8> x n   one frame, n = 8..64 bands, low to high;
 *                                            brightness 0 = the standby setting (anim.h)
 *   "LV" 00                                  stream ends (also after LIVE_TIMEOUT_MS of silence)
 *
 * The panel (40 MHz SPI on the Plus board, ~51 ms per full screen) gets only
 * what changed: per bar the rows between its old and new height / peak cap.
 * Host side: tools/ble_live.py.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

static const uint32_t LIVE_TIMEOUT_MS = 1500;   // no packet this long = stream over

void    live_packet(const uint8_t *data, size_t n); // BLE task: store the latest frame
bool    live_streaming();                           // packets are coming
bool    live_take_fresh();                          // a stream (re)started since the last call
uint8_t live_brightness();                          // requested by the computer, 0 = default

void live_start();                                  // takes over the panel (clears it)
void live_stop();                                   // gives the panel back to LVGL (full redraw)
bool live_active();
void live_loop();                                   // Arduino loop: draws while active
void live_redraw();                                 // clear and draw every bar again (the overlay changed)
