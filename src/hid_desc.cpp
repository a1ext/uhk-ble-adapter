#include "hid_desc.h"
#include <algorithm>

namespace {

enum : uint8_t { ITEM_MAIN = 0, ITEM_GLOBAL = 1, ITEM_LOCAL = 2 };

// Main item tags
enum : uint8_t { TAG_INPUT = 0x8, TAG_OUTPUT = 0x9, TAG_COLLECTION = 0xA,
                 TAG_FEATURE = 0xB, TAG_END_COLLECTION = 0xC };
// Global item tags
enum : uint8_t { TAG_USAGE_PAGE = 0x0, TAG_LOGICAL_MIN = 0x1, TAG_LOGICAL_MAX = 0x2,
                 TAG_REPORT_SIZE = 0x7, TAG_REPORT_ID = 0x8, TAG_REPORT_COUNT = 0x9,
                 TAG_PUSH = 0xA, TAG_POP = 0xB };
// Local item tags
enum : uint8_t { TAG_USAGE = 0x0, TAG_USAGE_MIN = 0x1, TAG_USAGE_MAX = 0x2 };

// Usage pages / usages the statistics look for
enum : uint32_t { PAGE_DESKTOP = 0x01, PAGE_KEYBOARD = 0x07, PAGE_BUTTON = 0x09, PAGE_CONSUMER = 0x0C,
                  USAGE_X = 0x00010030, USAGE_Y = 0x00010031 };

struct Item {
    uint8_t  type, tag, size;
    uint32_t u;     // data, zero-extended
    int32_t  s;     // data, sign-extended
    size_t   len;   // whole item length in bytes (prefix included)
};

// Decodes the item at d[pos]; returns false if truncated
bool nextItem(const uint8_t *d, size_t len, size_t pos, Item &it)
{
    uint8_t pfx = d[pos];
    if (pfx == 0xFE) { // long item: 0xFE, size, tag, data...
        if (pos + 3 > len || pos + 3 + d[pos + 1] > len)
            return false;
        it = {0xFF, 0xFF, d[pos + 1], 0, 0, (size_t)3 + d[pos + 1]};
        return true;
    }
    uint8_t sz = pfx & 3;
    if (sz == 3)
        sz = 4;
    if (pos + 1 + sz > len)
        return false;
    uint32_t u = 0;
    for (uint8_t k = 0; k < sz; k++)
        u |= (uint32_t)d[pos + 1 + k] << (8 * k);
    int32_t s = sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u;
    it = {(uint8_t)((pfx >> 2) & 3), (uint8_t)(pfx >> 4), sz, u, s, (size_t)1 + sz};
    return true;
}

struct Globals {
    uint16_t usagePage = 0;
    int32_t  logMin = 0;
    int32_t  logMaxS = 0;     // logical maximum, sign-extended
    uint32_t logMaxU = 0;     // logical maximum, zero-extended
    uint32_t reportSize = 0;
    uint32_t reportCount = 0;
    uint8_t  reportId = 0;
};

HidReportLayout &layoutFor(HidDescInfo &info, HidReportType type, uint8_t id)
{
    for (auto &r : info.reports)
        if (r.type == type && r.id == id)
            return r;
    info.reports.push_back({type, id, 0, {}, {}});
    return info.reports.back();
}

} // namespace

bool hid_parse(const uint8_t *d, size_t len, HidDescInfo &out)
{
    out = HidDescInfo();
    Globals g;
    std::vector<Globals> stack;
    int depth = 0;
    bool tlcSeen = false;
    uint32_t firstUsage = 0;
    uint8_t  firstUsageSize = 0;  // 0 = no usage since the last main item
    // Local usages of the next main item, with their usage page
    std::vector<uint32_t> usages;
    uint32_t usageMin = 0, usageMax = 0;
    bool haveMin = false, haveMax = false;
    auto usageOf = [&](size_t k) -> uint32_t { // element k's usage (the last one repeats)
        if (!usages.empty())
            return usages[std::min(k, usages.size() - 1)];
        if (haveMin && haveMax)
            return std::min(usageMin + (uint32_t)k, usageMax);
        return 0;
    };

    for (size_t pos = 0; pos < len;) {
        Item it;
        if (!nextItem(d, len, pos, it))
            return false;
        pos += it.len;

        if (it.type == ITEM_MAIN) {
            switch (it.tag) {
            case TAG_INPUT:
            case TAG_OUTPUT:
            case TAG_FEATURE: {
                HidReportType t = it.tag == TAG_INPUT ? HID_INPUT
                                : it.tag == TAG_OUTPUT ? HID_OUTPUT : HID_FEATURE;
                HidReportLayout &r = layoutFor(out, t, g.reportId);
                bool constant = it.u & 0x01, variable = it.u & 0x02, relative = it.u & 0x04;
                uint8_t count = std::min<uint32_t>(g.reportCount, 255);
                if (t == HID_INPUT && !constant && variable && relative
                        && g.reportSize >= 1 && g.reportSize <= 32 && g.reportCount) {
                    // Logical Maximum is unsigned when the minimum is non-negative
                    // (e.g. 0xFF in one byte means 255, not -1)
                    int32_t lmax = (g.logMin >= 0 && g.logMaxS < g.logMin)
                                 ? (int32_t)g.logMaxU : g.logMaxS;
                    uint8_t xy = 0;
                    for (uint8_t k = 0; k < count && k < 8; k++)
                        if (usageOf(k) == USAGE_X || usageOf(k) == USAGE_Y)
                            xy |= 1 << k;
                    r.rel.push_back({r.bits, (uint8_t)g.reportSize, count, g.logMin, lmax, xy});
                } else if (t == HID_INPUT && !constant && g.reportSize >= 1 && g.reportSize <= 16 && count) {
                    uint32_t u = usageOf(0);
                    uint32_t page = u ? u >> 16 : g.usagePage;
                    if (variable && g.reportSize == 1 && (page == PAGE_KEYBOARD || page == PAGE_BUTTON))
                        r.keys.push_back({r.bits, 1, count, false, page == PAGE_BUTTON ? HID_BUTTONS : HID_KEYS});
                    else if (!variable && (page == PAGE_KEYBOARD || page == PAGE_CONSUMER || page == PAGE_DESKTOP))
                        r.keys.push_back({r.bits, (uint8_t)g.reportSize, count, true, HID_KEYS});
                }
                r.bits += g.reportSize * g.reportCount;
                break;
            }
            case TAG_COLLECTION:
                if (depth == 0 && it.u == 0x01 /* application */ && !tlcSeen) {
                    tlcSeen = true;
                    // a 4-byte usage carries its own usage page in the upper half
                    out.tlcUsagePage = firstUsageSize == 4 ? firstUsage >> 16 : g.usagePage;
                    out.tlcUsage = firstUsage & 0xFFFF;
                }
                depth++;
                break;
            case TAG_END_COLLECTION:
                depth--;
                break;
            }
            firstUsageSize = 0; // local items only live until the next main item
            usages.clear();
            haveMin = haveMax = false;
        } else if (it.type == ITEM_GLOBAL) {
            switch (it.tag) {
            case TAG_USAGE_PAGE:   g.usagePage = it.u; break;
            case TAG_LOGICAL_MIN:  g.logMin = it.s; break;
            case TAG_LOGICAL_MAX:  g.logMaxS = it.s; g.logMaxU = it.u; break;
            case TAG_REPORT_SIZE:  g.reportSize = it.u; break;
            case TAG_REPORT_COUNT: g.reportCount = it.u; break;
            case TAG_REPORT_ID:
                g.reportId = it.u;
                out.usesIds = true;
                break;
            case TAG_PUSH:
                stack.push_back(g);
                break;
            case TAG_POP:
                if (stack.empty())
                    return false;
                g = stack.back();
                stack.pop_back();
                break;
            }
        } else if (it.type == ITEM_LOCAL) {
            // a 4-byte usage carries its own usage page
            uint32_t full = it.size == 4 ? it.u : ((uint32_t)g.usagePage << 16) | it.u;
            if (it.tag == TAG_USAGE) {
                if (!firstUsageSize) {
                    firstUsage = it.u;
                    firstUsageSize = it.size;
                }
                if (usages.size() < 64)
                    usages.push_back(full);
            } else if (it.tag == TAG_USAGE_MIN) {
                usageMin = full;
                haveMin = true;
            } else if (it.tag == TAG_USAGE_MAX) {
                usageMax = full;
                haveMax = true;
            }
        }
    }
    return depth == 0 && tlcSeen;
}

void hid_append_remapped(const uint8_t *d, size_t len, bool usesIds,
                         const uint8_t idMap[256], std::vector<uint8_t> &out)
{
    static const uint8_t RESET_GLOBALS[] = {
        0x15, 0x00, // Logical Minimum (0)
        0x35, 0x00, // Physical Minimum (0)
        0x45, 0x00, // Physical Maximum (0)
        0x55, 0x00, // Unit Exponent (0)
        0x65, 0x00, // Unit (None)
    };
    out.insert(out.end(), RESET_GLOBALS, RESET_GLOBALS + sizeof(RESET_GLOBALS));

    int depth = 0;
    bool idInserted = usesIds;
    for (size_t pos = 0; pos < len;) {
        Item it;
        if (!nextItem(d, len, pos, it))
            break;
        if (usesIds && it.type == ITEM_GLOBAL && it.tag == TAG_REPORT_ID && it.size == 1) {
            out.push_back(d[pos]);
            out.push_back(idMap[it.u & 0xFF]);
        } else {
            out.insert(out.end(), d + pos, d + pos + it.len);
        }
        if (it.type == ITEM_MAIN && it.tag == TAG_COLLECTION) {
            // The Report ID goes right inside the top-level collection:
            // Windows rejects one declared outside ("Report ID outside a
            // top-level collection", code 10)
            if (depth == 0 && !idInserted) {
                out.push_back(0x85); // Report ID
                out.push_back(idMap[0]);
                idInserted = true;
            }
            depth++;
        } else if (it.type == ITEM_MAIN && it.tag == TAG_END_COLLECTION) {
            depth--;
        }
        pos += it.len;
    }
}
