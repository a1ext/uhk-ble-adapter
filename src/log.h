/**
 * Logging: UART0 (Serial, CN1 connector), native USB CDC (USBSerial) while the
 * USB PHY is not taken by the USB host, and a short tail shown on the display.
 * Safe to call from any task (not from ISRs).
 */
#pragma once
#include <Arduino.h>

void log_begin();
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_hex(const char *prefix, const uint8_t *data, size_t len);

// Stop mirroring to the native USB CDC (call before the USB host is installed)
void log_disable_usb_cdc();

// Copies the on-screen log tail (newline separated) into buf.
// Returns true when it changed since the previous call.
bool log_screen_text(char *buf, size_t size);
