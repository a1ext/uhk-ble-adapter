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
#include "anim.h"
#include "ble_hid.h"
#include "bridge.h"
#include "config.h"
#include "display.h"
#include "live.h"
#include "log.h"
#include "ota.h"
#include "stats.h"
#include "ui.h"

static const char *bootNote = nullptr;
static uint32_t    lastActivityMs = 0;
static uint8_t     screenLevel = DISPLAY_BRIGHTNESS; // where the brightness is heading
static bool        standbyDone = false;  // the standby animation ran its course until the next activity
static bool        animByCommand = false; // started by the computer, not by idling
static uint32_t    liveSuppressUntil = 0; // the live spectrum waits after the user took the screen
static const uint32_t LIVE_RESUME_MS = 30000;

// The standby animation or the live spectrum owns the panel; LVGL is paused meanwhile
static bool panelTaken()
{
    return anim_playing() || live_active();
}

static void wakeScreen()
{
    if (live_active()) { // the user wants the normal screen; the spectrum returns after a pause
        live_stop();
        liveSuppressUntil = millis() + LIVE_RESUME_MS;
    }
    anim_stop();
    standbyDone = false;
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
// it: LVGL does not see it, so nothing on the screen is tapped blindly. The same
// for a touch that stops the standby animation.
static void pollTouch()
{
    static bool swallowing = false;
    int16_t x, y;
    bool touched = display_get_touch(&x, &y);
    if (touched) {
        if ((screenLevel == 0 || display_brightness() == 0 || anim_playing() || live_active()) && !swallowing) {
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

static uint8_t liveLevel()
{
    return live_brightness() ? live_brightness() : anim_brightness_now();
}

// The standby animation / live spectrum follow the time of day while they play
static void followDaylight(uint8_t level)
{
    if (level != screenLevel) {
        screenLevel = level;
        display_set_brightness(level);
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

    if (live_active()) { // streamed by the computer: no standby, no dimming
        followDaylight(liveLevel());
        return;
    }
    uint32_t idle = millis() - lastActivityMs;
    const AnimSettings &as = anim_settings();
    if (anim_playing()) {
        // Burn-in protection still applies to a standby animation left running
        if (!animByCommand && as.offAfterS && anim_played_ms() > as.offAfterS * 1000UL) {
            anim_stop();
            standbyDone = true;
            screenLevel = 0;
            display_fade_to(0, SCREEN_FADE_MS);
        } else {
            followDaylight(anim_brightness_now());
        }
        return;
    }
    if (as.mode == ANIM_MODE_STANDBY && anim_available() && !standbyDone && screenLevel > 0
        && idle > as.delayS * 1000UL) {
        animByCommand = false;
        anim_start();
        screenLevel = anim_brightness_now();
        display_set_brightness(screenLevel);
        return;
    }
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
    anim_begin();   // standby animation from the data partition
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

    switch (anim_loop()) {
    case ANIM_EV_PLAY: // from the computer: play now, until stopped or touched
        wakeScreen();
        animByCommand = true;
        anim_start();
        screenLevel = anim_brightness_now();
        display_set_brightness(screenLevel);
        break;
    case ANIM_EV_STOP:
        wakeScreen();
        break;
    default:
        break;
    }

    // Live spectrum: starts with the stream, ends when it stops
    OtaStatus ota;
    ota_get_status(ota);
    if (live_take_fresh())
        liveSuppressUntil = millis(); // a new stream shows right away
    if (!live_active() && live_streaming() && !ota.active
        && (int32_t)(millis() - liveSuppressUntil) >= 0) {
        anim_stop();
        standbyDone = false;
        live_start();
        screenLevel = liveLevel();
        display_set_brightness(screenLevel);
    } else if (live_active() && (!live_streaming() || ota.active)) {
        live_stop();
        wakeScreen();
    }
    live_loop();

    uint32_t now = millis();
    if (now >= nextUiMs) {
        nextUiMs = now + 100;
        updateScreenPower();
        if (!panelTaken())
            ui_update(bootNote);
    }
    display_loop();
    if (!panelTaken())
        lv_task_handler();
    delay(panelTaken() ? 1 : 5);
}
