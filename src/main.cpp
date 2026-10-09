/**
 * UHK 60 v2 -> BLE adapter (LilyGo T-Display-S3 AMOLED Plus)
 *
 * The keyboard plugs into the board's native USB port: the ESP32 is its USB
 * host and a BLE HID device to up to BLE_SLOTS computers / phones. Holding
 * BOOT starts BLE pairing on the active slot.
 */
#include <Arduino.h>
#include <esp_system.h>
#include <lvgl.h>
#include "ble_hid.h"
#include "bridge.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "ota.h"
#include "stats.h"
#include "ui.h"

static const char *bootNote = nullptr;
static uint32_t    lastActivityMs = 0;
static uint8_t     screenLevel = DISPLAY_BRIGHTNESS; // where the brightness is heading

static void wakeScreen()
{
    lastActivityMs = millis();
    screenLevel = DISPLAY_BRIGHTNESS;
    display_set_brightness(DISPLAY_BRIGHTNESS);
}

static void onSlotRequest(uint8_t slot)
{
    wakeScreen();
    ble_hid_select_slot(slot);
}

static void onPair()
{
    wakeScreen();
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

// Pairing button (BOOT): hold = pairing mode, press while pairing = cancel
struct Button {
    gpio_num_t pin;
    bool     stable;      // debounced state, true = pressed
    bool     lastRaw;
    uint32_t rawChangedMs;
    uint32_t downMs;
    bool     held;        // the hold action fired for this press
};
static Button buttons[] = {{PIN_BTN_BOOT}};

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

    // Shown only where holding leads somewhere: not while pairing (a press
    // then cancels it). A firmware update uses the same bar.
    BleHidStatus ble;
    ble_hid_get_status(ble);
    OtaStatus ota;
    ota_get_status(ota);
    uint16_t bar = ble.started && !ble.pairingLeftMs ? holdProgress : 0;
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
// it: LVGL does not see it, so nothing on the screen is tapped blindly.
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
    bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT
                || reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;

    log_begin();
    logf("UHK BLE adapter, fw %s %s, reset reason %d", __DATE__, __TIME__, reason);
    ota_init();
    stats_begin();
    if (crashed)
        bootNote = "restarted after a crash / watchdog / brownout";

    for (auto &b : buttons)
        pinMode(b.pin, INPUT_PULLUP);

    if (!display_begin())
        logf("Display init failed");
    bridge_begin(); // loads the cached HID map and the BLE host slots
    ui_begin({onSlotRequest, onPair, onPairCancel, onAgentAllow});
    // Draw the first frame now: BLE / USB host start below can take a while
    ui_update(bootNote);
    lv_refr_now(nullptr);

    bridge_start(); // USB host for the keyboard + BLE
    wakeScreen();
}

void loop()
{
    static uint32_t nextUiMs = 0;

    pollButtons();
    pollTouch();
    bridge_loop();
    ota_loop(ble_hid_ready());
    stats_loop(screenLevel == 0);

    uint32_t now = millis();
    if (now >= nextUiMs) {
        nextUiMs = now + 100;
        updateScreenPower();
        ui_update(bootNote);
    }
    display_loop();
    lv_task_handler();
    delay(5);
}
