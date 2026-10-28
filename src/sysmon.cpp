#include "sysmon.h"
#include <Arduino.h>

static portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;
static SysSample         latest;
static bool              fresh;
static volatile uint32_t lastPacketMs;
static volatile bool     ever;

static uint16_t u16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

void sysmon_packet(const uint8_t *d, size_t n)
{
    if (n < 17 || d[0] != 'S' || d[1] != 'Y' || d[2] != 0x01)
        return;
    SysSample s;
    s.cpu = d[3];
    s.ram = d[4];
    s.gpu = d[5];
    s.vram = d[6];
    s.netRx = u16(d + 7);
    s.netTx = u16(d + 9);
    s.diskR = u16(d + 11);
    s.diskW = u16(d + 13);
    s.cpuTemp = d[15];
    s.gpuTemp = d[16];
    portENTER_CRITICAL(&mux);
    latest = s;
    fresh = true;
    portEXIT_CRITICAL(&mux);
    lastPacketMs = millis();
    ever = true;
}

bool sysmon_take(SysSample &s)
{
    portENTER_CRITICAL(&mux);
    bool f = fresh;
    if (f)
        s = latest;
    fresh = false;
    portEXIT_CRITICAL(&mux);
    return f;
}

bool sysmon_streaming()
{
    return ever && millis() - lastPacketMs < SYS_TIMEOUT_MS;
}
