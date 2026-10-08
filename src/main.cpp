/**
 * UHK 60 v2 -> BLE adapter (LilyGo T-Display-S3 AMOLED Plus)
 *
 *   Cable mode: the USB 2:1 mux connects the keyboard straight to the PC,
 *               the ESP32 is not in the data path (UHK Agent works).
 *   BT mode:    the mux connects the keyboard to the ESP32 native USB; the
 *               ESP32 is its USB host and a BLE HID device to the PC.
 *
 * The mode is switched on the touch screen and kept in NVS; after a crash /
 * watchdog / brownout reset the adapter starts in cable mode. Holding BOOT (or
 * the panel button on GPIO10) starts BLE pairing on the active slot.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <lvgl.h>
#include "ble_hid.h"
#include "bridge.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "ota.h"
#include "ui.h"

static Preferences prefs;
static bool        btMode = false;      // applied mode
static bool        wantBt = false;      // requested mode, shown on screen
static bool        modePending = false;
static uint32_t    modeApplyAtMs = 0;
static const char *bootNote = nullptr;
static uint32_t    lastActivityMs = 0;
static uint8_t     screenLevel = DISPLAY_BRIGHTNESS; // where the brightness is heading

static void wakeScreen()
{
    lastActivityMs = millis();
    screenLevel = DISPLAY_BRIGHTNESS;
    display_set_brightness(DISPLAY_BRIGHTNESS);
}

static void setMode(bool bt, bool save)
{
    btMode = wantBt = bt;
    // Switch the keyboard first, then start / stop the USB host and BLE
    gpio_set_level(PIN_MUX_SEL, bt ? 1 : 0);
    bridge_set_bt_mode(bt);
    if (save)
        prefs.putUChar("mode", bt ? 1 : 0);
    logf("Mode: %s", bt ? "BLUETOOTH" : "CABLE");
    wakeScreen();
}

// Applied a moment later, so the switch animation is not stalled by the
// BLE / USB host start-up
static void onModeRequest(bool bt)
{
    wantBt = bt;
    modePending = true;
    modeApplyAtMs = millis() + MODE_SWITCH_DELAY_MS;
    wakeScreen();
}

static void onSlotRequest(uint8_t slot)
{
    wakeScreen();
    ble_hid_select_slot(slot);
}

static void onPair()
{
    wakeScreen();
    if (!btMode) {
        logf("Pairing: switch to Bluetooth mode first");
        return;
    }
    ble_hid_start_pairing();
}

static void onPairCancel()
{
    wakeScreen();
    ble_hid_stop_pairing();
}

static void onAgentAllow(bool allow)
{
    wakeScreen();
    bridge_agent_allow(allow);
}

// Physical pairing button(s): hold = pairing mode, press while pairing = cancel
struct Button {
    gpio_num_t pin;
    bool     stable;      // debounced state, true = pressed
    bool     lastRaw;
    uint32_t rawChangedMs;
    uint32_t downMs;
    bool     held;        // the hold action fired for this press
};
static Button buttons[] = {{PIN_BTN_BOOT}, {PIN_BTN_EXT}};

static void pollButtons()
{
    uint32_t now = millis();
    uint16_t holdProgress = 0;
    for (auto &b : buttons) {
        bool raw = gpio_get_level(b.pin) == 0;
        if (raw != b.lastRaw) {
            b.lastRaw = raw;
            b.rawChangedMs = now;
        }
        if (raw != b.stable && now - b.rawChangedMs >= BTN_DEBOUNCE_MS) {
            b.stable = raw;
            if (raw) {
                b.downMs = now;
                b.held = false;
                wakeScreen();
            } else if (!b.held) {
                BleHidStatus ble;
                ble_hid_get_status(ble);
                if (ble.pairingLeftMs)
                    onPairCancel();
            }
        }
        if (b.stable && !b.held && now - b.downMs >= LONG_PRESS_MS) {
            b.held = true;
            logf("Button GPIO%d held - pairing", b.pin);
            onPair();
        }
        if (b.stable && !b.held)
            holdProgress = max<uint16_t>(holdProgress, (now - b.downMs) * 1000 / LONG_PRESS_MS);
    }

    // Shown only where holding leads somewhere: BT mode, not pairing yet
    // (a press during pairing cancels it). A firmware update uses the same bar.
    BleHidStatus ble;
    ble_hid_get_status(ble);
    OtaStatus ota;
    ota_get_status(ota);
    uint16_t bar = btMode && !ble.pairingLeftMs ? holdProgress : 0;
    if (!bar && ota.active && ota.total)
        bar = max<uint16_t>(1, (uint64_t)ota.received * 1000 / ota.total);
    ui_set_hold_progress(bar);
}

static lv_indev_t *touchIndev()
{
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER)
            return i;
    return nullptr;
}

// Any touch is activity. A touch on a dark (or fading out) screen only wakes
// it: LVGL does not see it, so it cannot flip the switch blindly.
static void pollTouch()
{
    static bool swallowing = false;
    int16_t x, y;
    bool touched = display_get_touch(&x, &y);
    if (touched) {
        if ((screenLevel == 0 || display_brightness() == 0) && !swallowing) {
            swallowing = true;
            if (lv_indev_t *in = touchIndev()) {
                lv_indev_reset(in, nullptr);
                lv_indev_enable(in, false);
            }
        }
        wakeScreen();
    } else if (swallowing) {
        swallowing = false;
        if (lv_indev_t *in = touchIndev())
            lv_indev_enable(in, true);
    }
}

// Full brightness while something happens, dimmed / off when idle
static void updateScreenPower()
{
    static int lastSignature = -1;

    BridgeStatus bs;
    bridge_get_status(bs);
    BleHidStatus ble;
    ble_hid_get_status(ble);
    OtaStatus ota;
    ota_get_status(ota);
    int signature = bs.usbConnected | ble.connected << 1 | ble.ready << 2 | (bs.note != nullptr) << 3
                  | ota.active << 4 | ble.passkeyShown << 5 | bs.agentAsking << 6;
    if (signature != lastSignature) {
        lastSignature = signature;
        wakeScreen();
    }

    uint32_t idle = millis() - lastActivityMs;
    uint8_t level = idle > SCREEN_OFF_MS ? 0
                  : idle > SCREEN_DIM_MS ? DISPLAY_DIM_BRIGHTNESS : DISPLAY_BRIGHTNESS;
    if (level < screenLevel) { // only dims here, wakeScreen() brightens
        screenLevel = level;
        display_fade_to(level, SCREEN_FADE_MS);
    }
}

void setup()
{
    esp_reset_reason_t reason = esp_reset_reason();
    prefs.begin("adapter", false);
    bool savedBt = prefs.getUChar("mode", 0) != 0;
    bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT
                || reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;
    bool startBt = savedBt && !crashed;
    wantBt = startBt;

    // Mux select before anything else. After a deliberate restart the pad is
    // still held at its previous level (see bridge_loop); the hold is released
    // only once the new level is driven, so a BT -> BT restart is glitch-free.
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_MUX_SEL;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_set_level(PIN_MUX_SEL, startBt);
    gpio_config(&io);
    gpio_set_level(PIN_MUX_SEL, startBt);
    gpio_hold_dis(PIN_MUX_SEL);

    log_begin();
    logf("UHK BLE adapter, fw %s %s, reset reason %d, saved mode %s", __DATE__, __TIME__, reason,
         savedBt ? "BT" : "cable");
    ota_init();
    if (crashed && savedBt) {
        bootNote = "recovered from a crash: cable mode";
        logf("Crash / watchdog reset - starting in cable mode");
    }

    for (auto &b : buttons)
        pinMode(b.pin, INPUT_PULLUP);

    if (!display_begin())
        logf("Display init failed");
    bridge_begin(); // loads the cached HID map and the BLE host slots
    ui_begin({onModeRequest, onSlotRequest, onPair, onPairCancel, onAgentAllow});
    // Draw the first frame now: BLE / USB host start below can take a while
    ui_update(wantBt, bootNote);
    lv_refr_now(nullptr);

    setMode(startBt, false);
}

void loop()
{
    static uint32_t nextUiMs = 0;

    pollButtons();
    pollTouch();
    if (modePending && (int32_t)(millis() - modeApplyAtMs) >= 0) {
        modePending = false;
        if (wantBt != btMode)
            setMode(wantBt, true);
    }
    bridge_loop();
    ota_loop(ble_hid_ready(), btMode);

    uint32_t now = millis();
    if (now >= nextUiMs) {
        nextUiMs = now + 100;
        updateScreenPower();
        ui_update(wantBt, bootNote);
    }
    display_loop();
    lv_task_handler();
    delay(5);
}
