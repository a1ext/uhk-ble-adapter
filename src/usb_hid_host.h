/**
 * USB host for a composite HID keyboard (ESP-IDF 4.4 USB Host Library).
 *
 * Opens every HID interface (not just the boot keyboard), reads all report
 * descriptors, polls the interrupt IN endpoints selected by the consumer and
 * sends SET_REPORT requests (keyboard LEDs, feature reports) on its behalf.
 * Installing the host takes the native USB PHY away from USB-Serial-JTAG.
 */
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct UsbHidIface {
    uint8_t  num;            // bInterfaceNumber
    uint8_t  protocol;       // bInterfaceProtocol: 1 = boot keyboard, 2 = boot mouse
    uint8_t  epIn;           // interrupt IN endpoint address, 0 = none
    uint16_t mps;
    uint8_t  interval;
    uint16_t reportDescLen;  // from the HID class descriptor
    std::vector<uint8_t> reportDesc;
};

struct UsbHidDevice {
    uint16_t vid, pid;
    std::string product;
    std::vector<UsbHidIface> ifaces; // HID interfaces in descriptor order
};

// All callbacks run in the USB client task
struct UsbHidHostCallbacks {
    // Report descriptors are read. Returns a bit mask of indices into
    // dev.ifaces whose input endpoints should be polled.
    uint32_t (*onDeviceReady)(const UsbHidDevice &dev);
    void (*onInputReport)(uint8_t ifaceIdx, const uint8_t *data, uint16_t len);
    void (*onDeviceGone)();
    // Backpressure: false postpones polling until the consumer has room again
    bool (*canAcceptReport)();
};

void usb_hid_host_begin(const UsbHidHostCallbacks &cb);
bool usb_hid_host_started();

// Queues a SET_REPORT (2 = output, 3 = feature) for interface index ifaceIdx;
// reportId 0 = the interface uses no report IDs. Callable from any task.
bool usb_hid_host_set_report(uint8_t ifaceIdx, uint8_t reportType, uint8_t reportId,
                             const uint8_t *payload, uint16_t len);
