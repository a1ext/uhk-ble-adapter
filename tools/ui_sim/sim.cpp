// Renders the adapter's real UI (src/ui.cpp) and live spectrum (src/live.cpp)
// on a PC with canned data and writes raw frames <name>.rgb (536 x 240, RGB888)
// into the current directory. Built and run by render.py.
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "anim.h"
#include "ble_hid.h"
#include "bridge.h"
#include "cal.h"
#include "config.h"
#include "display.h"
#include "live.h"
#include "log.h"
#include "ota.h"
#include "overlay.h"
#include "stats.h"
#include "sysmon.h"
#include "ui.h"

// ---------------- Stubs for what ui.cpp asks the firmware ----------------
static uint32_t nowMs = 0;
extern "C" uint32_t millis(void) { return nowMs; }

static BleHidStatus ble;
static BridgeStatus br;
static OtaStatus ota;
static const char *logText = "";

void ble_hid_get_status(BleHidStatus &st) { st = ble; }
void ble_hid_set_host_name(uint8_t slot, const char *name) { strcpy(ble.slotName[slot], name); }
static AnimSettings animSettings = {ANIM_MODE_STANDBY, 60, 180, 1800, 30};
const AnimSettings &anim_settings() { return animSettings; }
void anim_set_settings(const AnimSettings &s) { animSettings = s; }
bool anim_available() { return true; }
// 14:37 on Wednesday 28 October 2026
bool anim_clock_now(uint8_t &h, uint8_t &m, uint8_t &s) { h = 14; m = 37; s = 0; return true; }
bool display_rtc_read_date(uint16_t &y, uint8_t &mo, uint8_t &d) { y = 2026; mo = 10; d = 28; return true; }
// Calendar: one canned meeting, the reminder switched on by the scene
static CalEvent meeting;
static bool     haveMeeting = false, remindNow = false;
bool cal_has_data() { return haveMeeting; }
bool cal_next(CalEvent &e) { e = meeting; return haveMeeting; }
bool cal_reminder(CalEvent &e) { e = meeting; return haveMeeting && remindNow; }
int32_t cal_minutes_to(const CalEvent &e)
{
    int32_t d = (int32_t)(e.startMs - nowMs);
    return d > 0 ? (d + 59999) / 60000 : -((-d) / 60000);
}
void cal_dismiss(uint16_t) {}
static void setMeeting(uint16_t id, int32_t startInMin, uint16_t durMin, uint8_t flags, const char *title)
{
    meeting.id = id;
    meeting.startMs = nowMs + startInMin * 60000L;
    meeting.durMin = durMin;
    meeting.flags = flags;
    snprintf(meeting.title, sizeof(meeting.title), "%s", title);
    haveMeeting = true;
}
// System load: samples fed by the dashboard scene
static SysSample sysSample;
static bool      sysFresh = false;
bool sysmon_take(SysSample &s) { s = sysSample; bool f = sysFresh; sysFresh = false; return f; }
bool sysmon_streaming() { return true; }
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

// ---------------- Raw panel for live.cpp (byte-swapped RGB565, like the panel) ----------------
static uint16_t panel[W * H];
uint16_t display_width() { return W; }
uint16_t display_height() { return H; }
static void rawPanel(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *px)
{
    for (int r = 0; r < h; r++)
        memcpy(&panel[(y + r) * W + x], px + r * w, 2 * w);
}
void display_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *px)
{
    overlay_push(x, y, w, h, px, rawPanel);   // the meeting reminder blends in, like on the adapter
}
void display_overlay_show() { overlay_show(rawPanel); }
void display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    static uint16_t black[W * 16];
    for (uint16_t yy = y; yy < y + h; yy += 16)
        display_push(x, yy, w, yy + 16 <= y + h ? 16 : y + h - yy, black);
}

static void shotPanel(const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), "%s.rgb", name);
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < W * H; i++) {
        uint16_t v = panel[i] >> 8 | panel[i] << 8;
        uint8_t r = v >> 11, g = v >> 5 & 63, b = v & 31;
        uint8_t rgb[3] = {(uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", path);
}

// Music-like band levels: kick drum in the bass, a vocal hump, falling treble
static void liveFrame(int frame, int kickFrame)
{
    const int N = 48;
    uint8_t pkt[3 + 1 + N] = {'L', 'V', 0x01, 0};
    float since = (frame - kickFrame) * 16.0f;
    float kick = since >= 0 ? expf(-since / 140.0f) : 0;
    for (int b = 0; b < N; b++) {
        float x = (float)b / (N - 1);
        float shape = 0.62f * expf(-x * 9) + 0.55f * expf(-powf((x - 0.45f) / 0.16f, 2)) + 0.38f * (1 - x) + 0.08f;
        float wobble = 0.12f * sinf(b * 1.7f + frame * 0.31f) + 0.08f * sinf(b * 3.1f - frame * 0.53f);
        float lv = shape * (0.72f + 0.28f * kick * (1.2f - x)) + wobble;
        pkt[4 + b] = (uint8_t)fminf(255, fmaxf(0, lv * 255));
    }
    live_packet(pkt, sizeof(pkt));
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
    // Made-up addresses (the screenshots are published)
    static const uint8_t hosts[BLE_SLOTS][6] = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
                                                {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB},
                                                {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    ble.started = ble.enabled = ble.connected = ble.ready = true;
    ble.mtu = 185;
    ble.interval = 6;
    ble.slot = 2;
    static const char *names[] = {"Work PC", "Pixel 8", "MacBook Pro"};
    for (int s = 0; s < 3; s++) {
        ble.slotUsed[s] = true;
        memcpy(ble.slotHost[s], hosts[s], 6);
        strcpy(ble.slotName[s], names[s]);
    }
    br.usbConnected = true;
    strcpy(br.usbProduct, "UHK 60 v2");
    br.mapValid = true;
    br.forwarded = 18342;
    br.merged = 2211;
    br.agentChannel = true;
    logText = "BLE: HID service up as 37A8:0003, 9 reports, report map 287 bytes + Agent service\n"
              "BLE: waiting for the slot 3 host\n"
              "BLE: host AA:BB:CC:DD:EE:FF connected\n"
              "BLE: link encrypted (bonded)\n"
              "BLE: slot 3 host authenticated\n"
              "BLE: MTU 185\n"
              "BLE: connection interval 7.50 ms";
    ui_update(nullptr);
    run(1500);
    shot("screen-bt");

    // Tap slot 5: the spark runs down the bus, then the pad lights up
    lv_obj_t *strip = lv_obj_get_child(lv_scr_act(), 1);
    lv_event_send(lv_obj_get_child(strip, 4), LV_EVENT_SHORT_CLICKED, nullptr);
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
              "BLE: host 11:22:33:44:55:66 connected";
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

    // Settings menu of slot 3 (a tap on its pad), then its rename keyboard
    br.agentAsking = false;
    ui_update(nullptr);
    lv_event_send(lv_obj_get_child(strip, 2), LV_EVENT_SHORT_CLICKED, nullptr);
    ui_update(nullptr);
    run(500);
    shot("screen-menu");
    // sys layer: the menu shade (its card is the shade's child), the rename panel
    lv_obj_t *card = lv_obj_get_child(lv_obj_get_child(lv_layer_sys(), 0), 0);
    lv_obj_t *rename = lv_obj_get_child(lv_obj_get_child(card, 1), 1);
    lv_event_send(rename, LV_EVENT_CLICKED, nullptr);
    run(300);
    shot("screen-rename");
    ui_close_menu();

    // A confirmed meeting in 12 minutes: the reminder card over the main screen
    setMeeting(7, 12, 30, CAL_CONFIRMED, "Design review");
    remindNow = true;
    ui_reminder_loop(false);
    ui_update(nullptr);
    run(300);
    shot("screen-remind");

    // Dashboard: the standby screen with the clock, the next meeting (42 min
    // away) and a minute of the PC's load
    remindNow = false;
    meeting.startMs = nowMs + 42 * 60000UL;
    ui_reminder_loop(false);
    ui_show_dashboard(true);
    for (int k = 0; k < 60; k++) {
        float t = k / 59.0f;
        sysSample.cpu = 18 + 30 * fabsf(sinf(k * 0.35f)) + (k > 40 ? 35 : 0);
        sysSample.ram = 61 + k / 12;
        sysSample.gpu = k < 25 ? 4 + (k % 5) : 55 + 30 * fabsf(sinf(k * 0.5f));
        sysSample.vram = 38 + (k > 25 ? 21 : 0);
        sysSample.netRx = 120 + 900 * fabsf(sinf(k * 0.22f)) * (k > 30 ? 1 : 0.2f);
        sysSample.netTx = 40 + 60 * t;
        sysSample.diskR = k % 17 == 0 ? 42 : 2;
        sysSample.diskW = k % 11 == 0 ? 18 : 1;
        sysSample.cpuTemp = 54 + sysSample.cpu / 5;
        sysSample.gpuTemp = 41 + sysSample.gpu / 3;
        sysFresh = true;
        ui_update(nullptr);
        run(500);
    }
    shot("screen-dash");
    ui_show_dashboard(false);

    // Live spectrum: a second of music at 60 fps, the shot ~0.2 s after a kick,
    // so the peak caps float above the falling bars
    live_start();
    for (int f = 0; f <= 58; f++) {
        liveFrame(f, f < 45 ? 20 : 45);
        live_loop();
        nowMs += 16;
    }
    shotPanel("screen-live");

    // ... with a tentative meeting's reminder composited over the bars
    setMeeting(8, 4, 60, CAL_TENTATIVE, "1:1 with Anna");
    remindNow = true;
    if (ui_reminder_loop(true))
        live_redraw();
    for (int f = 59; f <= 75; f++) {
        liveFrame(f, 45);
        live_loop();
        nowMs += 16;
    }
    shotPanel("screen-live-remind");
    return 0;
}
