/**
 * Minimal HID report descriptor parser / rewriter.
 *
 * The BLE report map is built by concatenating the keyboard's USB report
 * descriptors (one per HID interface) with the report IDs renumbered so they
 * are unique across interfaces. The parser extracts what the bridge needs:
 * report IDs, report sizes, and the relative input fields (mouse X/Y/wheel)
 * that may be summed when several USB reports are merged into one BLE report.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>

enum HidReportType : uint8_t { HID_INPUT = 1, HID_OUTPUT = 2, HID_FEATURE = 3 };

struct HidRelField {        // relative input field (array of `count` elements)
    uint16_t bitOffset;     // from the start of the payload (report ID byte excluded)
    uint8_t  bitSize;
    uint8_t  count;
    int32_t  logMin, logMax;
};

struct HidReportLayout {
    HidReportType type;
    uint8_t  id;            // report ID inside the interface, 0 = interface uses no IDs
    uint16_t bits;          // payload size in bits (report ID byte excluded)
    std::vector<HidRelField> rel;
    uint16_t bytes() const { return (bits + 7) / 8; }
};

struct HidDescInfo {
    bool     usesIds = false;
    uint16_t tlcUsagePage = 0;  // usage of the first top-level application collection
    uint16_t tlcUsage = 0;
    std::vector<HidReportLayout> reports;

    bool isVendor() const { return tlcUsagePage >= 0xFF00; }
};

// Returns false on a malformed descriptor
bool hid_parse(const uint8_t *desc, size_t len, HidDescInfo &out);

// Appends `desc` to `out` with report IDs renumbered: every Report ID item
// gets idMap[oldId]; an interface without IDs gets a Report ID item idMap[0]
// right after its top-level Collection item (Windows rejects a Report ID
// outside a top-level collection). Globals that could leak from the previous
// interface's descriptor (physical range, unit, unit exponent, logical
// minimum) are reset first.
void hid_append_remapped(const uint8_t *desc, size_t len, bool usesIds,
                         const uint8_t idMap[256], std::vector<uint8_t> &out);
