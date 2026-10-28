/**
 * System glance: the paired computer's load, streamed for the dashboard
 * (the standby screen with the clock, ui.cpp).
 *
 * Packets on the OTA data characteristic while no transfer runs (like the
 * live spectrum, live.h; write without response, ~2 per second):
 *   "SY" 01 <cpu %> <ram %> <gpu %> <vram %> <net rx KB/s u16> <net tx KB/s u16>
 *           <disk read 100 KB/s u16> <disk write 100 KB/s u16> <cpu temp C> <gpu temp C>
 *   0xFF / 0xFFFF = not available. Host side: tools/sysstats.py through ble_live.py.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

static const uint32_t SYS_TIMEOUT_MS = 5000;   // no packet this long = the computer stopped

struct SysSample {
    uint8_t  cpu, ram, gpu, vram, cpuTemp, gpuTemp;
    uint16_t netRx, netTx, diskR, diskW;
};

void sysmon_packet(const uint8_t *data, size_t n);  // BLE task
bool sysmon_take(SysSample &s);                      // a new sample since the last call
bool sysmon_streaming();                             // packets are coming
