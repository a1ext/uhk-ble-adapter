// Renders the adapter's real UI (src/ui.cpp) on a PC with canned status data
// and writes raw frames <name>.rgb (536 x 240, RGB888) into the current
// directory. Built and run by render.py.
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "ble_hid.h"
#include "bridge.h"
#include "config.h"
#include "log.h"
#include "ota.h"
#include "stats.h"
#include "ui.h"

// ---------------- Stubs for what ui.cpp asks the firmware ----------------
static uint32_t nowMs = 0;
extern "C" uint32_t millis(void) { return nowMs; }

static BleHidStatus ble;
static BridgeStatus br;
static OtaStatus ota;
static const char *logText = "";

void ble_hid_get_status(BleHidStatus &st) { st = ble; }
void bridge_get_status(BridgeStatus &st) { st = br; }
void ota_get_status(OtaStatus &st) { st = ota; }
// 48 213 keys, 6 702 clicks, 1.20 km of mouse
void stats_get(StatsSnapshot &st) { st = {48213, 6702, 1200.0 / 0.0254 * MOUSE_CPI}; }
float stats_meters(const StatsSnapshot &st) { return st.travel / MOUSE_CPI * 0.0254; }
bool log_screen_text(char *buf, size_t size)
{
    static const char *shown = nullptr;
    if (shown == logText)
        return false;
    shown = logText;
    snprintf(buf, size, "%s", logText);
    return true;
}

// ---------------- Headless display ----------------
static const int W = 536, H = 240;
static lv_color_t fb[W * H];

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = *px++;
    lv_disp_flush_ready(drv);
}

// Advances the clock in 5 ms steps, like the firmware loop
static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) {
        nowMs += 5;
        lv_timer_handler();
    }
}

static void shot(const char *name)
{
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    char path[64];
    snprintf(path, sizeof(path), "%s.rgb", name);
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < W * H; i++) {
        uint8_t rgb[3] = {(uint8_t)(fb[i].ch.red << 3 | fb[i].ch.red >> 2),
                          (uint8_t)(fb[i].ch.green << 2 | fb[i].ch.green >> 4),
                          (uint8_t)(fb[i].ch.blue << 3 | fb[i].ch.blue >> 2)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", path);
}

static void nop(bool) {}
static void nopSlot(uint8_t) {}
static void nopVoid() {}

// ---------------- Scenes ----------------
int main()
{
    lv_init();
    static lv_disp_draw_buf_t db;
    static lv_color_t buf[W * H];
    lv_disp_draw_buf_init(&db, buf, nullptr, W * H);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = W;
    drv.ver_res = H;
    drv.flush_cb = flush;
    drv.draw_buf = &db;
    drv.full_refresh = 1;
    lv_disp_drv_register(&drv);

    ui_begin({nopSlot, nopVoid, nopVoid, nop});

    // Keyboard attached, slots 1-3 paired, slot 3's host connected
    static const uint8_t hosts[BLE_SLOTS][6] = {{0x3C, 0x22, 0xFB, 0x1A, 0x07, 0x5E},
                                                {0xD4, 0x61, 0x9D, 0x40, 0xB2, 0x13},
                                                {0x84, 0x7B, 0x57, 0x2C, 0x9E, 0xA1}};
    ble.started = ble.enabled = ble.connected = ble.ready = true;
    ble.mtu = 185;
    ble.interval = 6;
    ble.slot = 2;
    for (int s = 0; s < 3; s++) {
        ble.slotUsed[s] = true;
        memcpy(ble.slotHost[s], hosts[s], 6);
    }
    br.usbConnected = true;
    strcpy(br.usbProduct, "UHK 60 v2");
    br.mapValid = true;
    br.forwarded = 18342;
    br.merged = 2211;
    br.agentChannel = true;
    logText = "BLE: HID service up as 37A8:0003, 9 reports, report map 287 bytes + Agent service\n"
              "BLE: waiting for the slot 3 host\n"
              "BLE: host 84:7B:57:2C:9E:A1 connected\n"
              "BLE: link encrypted (bonded)\n"
              "BLE: slot 3 host authenticated\n"
              "BLE: MTU 185\n"
              "BLE: connection interval 7.50 ms";
    ui_update(nullptr);
    run(1500);
    shot("screen-bt");

    // Tap slot 5: the spark runs down the bus, then the pad lights up
    lv_obj_t *strip = lv_obj_get_child(lv_scr_act(), 1);
    lv_event_send(lv_obj_get_child(strip, 4), LV_EVENT_CLICKED, nullptr);
    run(160);
    shot("anim-spark");
    run(260);
    shot("anim-pop");
    ble.slot = 4;
    ui_update(nullptr);
    run(1000);
    ble.slot = 2;
    ui_update(nullptr);
    run(1000);

    // Pairing a new device on slot 4: passkey pop-up
    ble.slot = 3;
    ble.ready = false;
    ble.pairingLeftMs = 14000;
    ble.passkeyShown = true;
    ble.passkey = 482913;
    logText = "Button GPIO0 held - pairing\n"
              "BLE: slot 4 pairing mode for 20 s\n"
              "BLE: discoverable as \"UHK60 BLE 4\" - pair it on the new device\n"
              "BLE: host 5A:13:C8:E2:40:7F connected";
    ui_update(nullptr);
    run(1500);
    shot("screen-passkey");

    // UHK Agent asks for config access
    ble.slot = 2;
    ble.ready = true;
    ble.pairingLeftMs = 0;
    ble.passkeyShown = false;
    br.agentAsking = true;
    logText = "BLE: slot 3 host authenticated\n"
              "BLE: MTU 185\n"
              "BLE: connection interval 7.50 ms\n"
              "Agent: config access refused - allow it on the screen";
    ui_update(nullptr);
    run(1500);
    shot("screen-agent");
    return 0;
}
