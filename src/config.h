/**
 * Pins, names and tunables of the UHK -> BLE adapter.
 */
#pragma once
#include <Arduino.h>

// ---------------- Pins (LilyGo T-Display-S3 AMOLED Plus) ----------------
// USB 2:1 mux select. LOW = cable mode (keyboard <-> PC), HIGH = BT mode
// (keyboard <-> ESP32 USB host). An external 10k pull-down keeps the mux in
// cable mode during reset, in the ROM bootloader and with dead firmware.
static constexpr gpio_num_t PIN_MUX_SEL  = GPIO_NUM_16;
// Pairing button: hold = pairing mode for the active slot, press while pairing = cancel
static constexpr gpio_num_t PIN_BTN_BOOT = GPIO_NUM_0;  // on-board BOOT key (10k pull-up + 100nF)
static constexpr gpio_num_t PIN_BTN_EXT  = GPIO_NUM_10; // optional panel button to GND

// ---------------- Buttons / touch UI ----------------
static constexpr uint32_t BTN_DEBOUNCE_MS      = 30;
static constexpr uint32_t MODE_SWITCH_DELAY_MS = 250;   // let the switch animation finish first
static constexpr uint32_t LONG_PRESS_MS        = 1500;  // pairing button hold time (also on screen)
static constexpr uint32_t PAIRING_WINDOW_MS    = 20 * 1000UL; // discoverable after "pair"

// ---------------- Display ----------------
static constexpr uint8_t  DISPLAY_ROTATION   = 0;       // 0/2 = landscape 536x240, 1/3 = portrait
static constexpr uint8_t  DISPLAY_BRIGHTNESS     = 160; // 0-255
static constexpr uint8_t  DISPLAY_DIM_BRIGHTNESS = 51;  // 20%
// AMOLED burn-in protection: dim, then off after the last activity
// (touch, button, keyboard / BLE connection change)
static constexpr uint32_t SCREEN_DIM_MS = 60 * 1000UL;
static constexpr uint32_t SCREEN_OFF_MS = 3 * 60 * 1000UL;
static constexpr uint32_t SCREEN_FADE_MS = 2500; // dim / off fade, slowing down towards the end

// ---------------- BLE ----------------
// Host slots: each slot is a separate BLE device ("UHK60 BLE 1".."5") with its
// own static random address, so every computer / phone pairs with its own slot
static constexpr uint8_t     BLE_SLOTS        = 5;
static constexpr const char *BLE_DEVICE_NAME  = "UHK60 BLE";
static constexpr const char *BLE_MANUFACTURER = "DIY";
// UHK Agent over BLE: the keyboard's vendor command interface (usage page
// 0xFF00, usage 1) gets its own HID service (a separate HID device on the
// host, no report IDs - like the USB interface), and the BLE PnP ID is the
// keyboard's own USB VID:PID, so Agent recognizes it as the keyboard.
// Read-only status commands pass; the user config (read / write), macros and
// other state changes need "Allow" on the screen; firmware updates are refused.
static constexpr bool     BLE_AGENT_CHANNEL = true;
// Agent config access lasts this long after its last command (and ends when
// the host disconnects); the "allow?" prompt stays up this long
static constexpr uint32_t AGENT_UNLOCK_MS = 10 * 60 * 1000UL;
static constexpr uint32_t AGENT_PROMPT_MS = 30 * 1000UL;
// PnP ID when the Agent channel is off: deliberately not the keyboard's
static constexpr uint16_t BLE_PNP_VID = 0x303A;         // Espressif
static constexpr uint16_t BLE_PNP_PID = 0x8360;
static constexpr uint16_t BLE_LOCAL_MTU = 185;          // NKRO report is 29 bytes, needs MTU >= 32
static constexpr uint8_t  BLE_MAX_IN_FLIGHT = 2;        // notifications queued in the controller
