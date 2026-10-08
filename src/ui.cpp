#include "ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include "ble_hid.h"
#include "bridge.h"
#include "config.h"
#include "log.h"
#include "ota.h"

static const uint32_t COLOR_CABLE  = 0x30D158; // iOS green
static const uint32_t COLOR_BT     = 0x0A84FF; // iOS blue
static const uint32_t COLOR_TEXT   = 0xE0E0E0;
static const uint32_t COLOR_DIM    = 0x707880;
static const uint32_t COLOR_OFF    = 0x48484A; // inactive mode label, free slot
static const uint32_t COLOR_USED   = 0xAEAEB2; // slot with a paired host
static const uint32_t COLOR_NOTE   = 0xFFA000;
static const uint32_t COLOR_RED    = 0xFF453A;
static const uint32_t COLOR_TRACK  = 0x1C1C1E;
static const uint32_t COLOR_NEON   = 0xFF2EC4; // slot bubble
static const uint32_t COLOR_NEON_HI = 0xFF9BE6;

static const lv_coord_t STRIP_W  = 72;
static const lv_coord_t BUBBLE_D = 58;
static const uint32_t   BUBBLE_ANIM_MS = 650;

static UiCallbacks cbs;
static lv_obj_t *modeSwitch, *cableLabel, *btLabel;
static lv_obj_t *usbLabel, *bleLabel, *slotRow, *slotLabel, *pairBtn, *pairLabel;
static bool pairingShown = false;
static lv_obj_t *statsLabel, *noteLabel, *logLabel;
static lv_obj_t *strip, *bubble, *slotNum[BLE_SLOTS];
static lv_obj_t *holdBar;
static lv_obj_t *passkeyCard, *passkeyLabel, *agentCard, *agentBtn, *agentLabel;
static lv_coord_t cellH;
static uint8_t shownSlot = 0xFF;

// ---------------- Helpers ----------------
static lv_obj_t *makeContainer(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, 4, 0);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void setText(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static void setVisible(lv_obj_t *o, bool visible)
{
    if (visible == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (visible)
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------- Mode switch ----------------
static void switchEvent(lv_event_t *)
{
    if (cbs.onModeRequest)
        cbs.onModeRequest(lv_obj_has_state(modeSwitch, LV_STATE_CHECKED));
}

// Tapping "Cable" / "Bluetooth" flips the switch to that side (animated)
static void modeLabelEvent(lv_event_t *e)
{
    bool bt = lv_event_get_user_data(e) != nullptr;
    if (bt == lv_obj_has_state(modeSwitch, LV_STATE_CHECKED))
        return;
    if (bt)
        lv_obj_add_state(modeSwitch, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(modeSwitch, LV_STATE_CHECKED);
    lv_event_send(modeSwitch, LV_EVENT_VALUE_CHANGED, nullptr); // animates + switchEvent
}

static lv_obj_t *makeModeLabel(lv_obj_t *parent, const char *text, bool bt)
{
    lv_obj_t *l = makeLabel(parent, &lv_font_montserrat_22, COLOR_OFF);
    lv_label_set_text(l, text);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, 12);
    lv_obj_add_event_cb(l, modeLabelEvent, LV_EVENT_CLICKED, bt ? (void *)1 : nullptr);
    return l;
}

// iOS-style switch: green track = cable, blue = Bluetooth, white knob
static lv_obj_t *makeModeSwitch(lv_obj_t *parent)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 112, 60);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_anim_time(sw, 200, LV_PART_MAIN);

    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_CABLE), LV_PART_MAIN);

    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_BT), LV_PART_INDICATOR | LV_STATE_CHECKED);

    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB); // knob inset from the track edge
    lv_obj_set_style_shadow_width(sw, 12, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sw, LV_OPA_50, LV_PART_KNOB);

    lv_obj_add_event_cb(sw, switchEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

// ---------------- Slot strip ----------------
static lv_coord_t bubbleY(uint8_t slot)
{
    return slot * cellH + (cellH - BUBBLE_D) / 2;
}

static void bubbleYCb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

// Jumps the neon bubble to a slot; animated it drops in and bounces
static void moveBubble(uint8_t slot, bool animate)
{
    lv_anim_del(bubble, bubbleYCb);
    if (!animate) {
        lv_obj_set_y(bubble, bubbleY(slot));
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bubble);
    lv_anim_set_exec_cb(&a, bubbleYCb);
    lv_anim_set_values(&a, lv_obj_get_y(bubble), bubbleY(slot));
    lv_anim_set_time(&a, BUBBLE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_bounce);
    lv_anim_start(&a);
}

static void slotEvent(lv_event_t *e)
{
    uint8_t slot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (slot == shownSlot)
        return;
    shownSlot = slot;
    moveBubble(slot, true);
    if (cbs.onSlotRequest)
        cbs.onSlotRequest(slot);
}

static void makeStrip(lv_obj_t *parent)
{
    strip = lv_obj_create(parent);
    lv_obj_set_size(strip, STRIP_W, lv_pct(100));
    lv_obj_set_style_bg_color(strip, lv_color_hex(COLOR_TRACK), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_radius(strip, STRIP_W / 2, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Neon tube: hot pink fill, light rim, wide pink glow
    bubble = lv_obj_create(strip);
    lv_obj_set_size(bubble, BUBBLE_D, BUBBLE_D);
    lv_obj_set_x(bubble, (STRIP_W - BUBBLE_D) / 2);
    lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 2, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(COLOR_NEON_HI), 0);
    lv_obj_set_style_shadow_color(bubble, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_shadow_width(bubble, 28, 0);
    lv_obj_set_style_shadow_spread(bubble, 2, 0);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_80, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Tap targets with the slot numbers, above the bubble
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        lv_obj_t *cell = lv_obj_create(strip);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cell, slotEvent, LV_EVENT_CLICKED, (void *)(uintptr_t)s);
        slotNum[s] = makeLabel(cell, &lv_font_montserrat_28, COLOR_OFF);
        lv_label_set_text_fmt(slotNum[s], "%u", s + 1);
        lv_obj_center(slotNum[s]);
    }
}

// Cell geometry needs the strip's final height
static void layoutStrip()
{
    lv_obj_update_layout(lv_scr_act());
    cellH = lv_obj_get_content_height(strip) / BLE_SLOTS;
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        lv_obj_t *cell = lv_obj_get_parent(slotNum[s]);
        lv_obj_set_size(cell, STRIP_W, cellH);
        lv_obj_set_pos(cell, 0, s * cellH);
    }
}

// ---------------- Pairing ----------------
// Hold = start pairing mode, tap while pairing = cancel
static void pairEvent(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (!pairingShown && cbs.onPair)
            cbs.onPair();
    } else if (pairingShown && cbs.onPairCancel) {
        cbs.onPairCancel();
    }
}

static lv_obj_t *makePairButton(lv_obj_t *parent)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 5, 0);

    pairLabel = makeLabel(b, &lv_font_montserrat_12, COLOR_NEON_HI);
    lv_obj_center(pairLabel);

    lv_obj_add_event_cb(b, pairEvent, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(b, pairEvent, LV_EVENT_SHORT_CLICKED, nullptr);
    return b;
}

// ---------------- Hold-to-pair progress ----------------
// Gradient bar along the bottom edge (top layer) while the pairing button is held
static void makeHoldBar()
{
    holdBar = lv_bar_create(lv_layer_top());
    lv_obj_set_size(holdBar, lv_disp_get_hor_res(nullptr) - 16, 8);
    lv_obj_align(holdBar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(holdBar, 0, 1000);
    lv_obj_clear_flag(holdBar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_radius(holdBar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(holdBar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(holdBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_anim_time(holdBar, 0, LV_PART_MAIN);

    lv_obj_set_style_radius(holdBar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(holdBar, lv_color_hex(COLOR_NEON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(holdBar, lv_color_hex(COLOR_BT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(holdBar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(holdBar, lv_color_hex(COLOR_NEON), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(holdBar, 14, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(holdBar, LV_OPA_60, LV_PART_INDICATOR);

    lv_obj_add_flag(holdBar, LV_OBJ_FLAG_HIDDEN);
}

// ---------------- Pop-ups (top layer) ----------------
// Dark card with a neon rim and glow, centered over everything
static lv_obj_t *makeCard()
{
    lv_obj_t *c = lv_obj_create(lv_layer_top());
    lv_obj_set_size(c, lv_disp_get_hor_res(nullptr) - 96, LV_SIZE_CONTENT);
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x111114), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_shadow_width(c, 30, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

// Pairing passkey: the host types it
static void makePasskeyCard()
{
    passkeyCard = makeCard();
    lv_obj_t *t = makeLabel(passkeyCard, &lv_font_montserrat_16, COLOR_DIM);
    lv_label_set_text(t, LV_SYMBOL_BLUETOOTH " Pairing code");
    passkeyLabel = makeLabel(passkeyCard, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_style_text_letter_space(passkeyLabel, 4, 0);
    lv_obj_t *h = makeLabel(passkeyCard, &lv_font_montserrat_14, COLOR_NEON_HI);
    lv_label_set_text(h, "Type it on the device you are pairing");
}

static void agentEvent(lv_event_t *e)
{
    if (cbs.onAgentAllow)
        cbs.onAgentAllow(lv_event_get_user_data(e) != nullptr);
}

static lv_obj_t *makeCardButton(lv_obj_t *parent, const char *text, bool primary)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, primary ? 18 : 0, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? COLOR_NEON : COLOR_TRACK), 0);
    if (primary) {
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(COLOR_BT), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
    }
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = makeLabel(b, &lv_font_montserrat_18, primary ? 0xFFFFFF : COLOR_TEXT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, agentEvent, LV_EVENT_CLICKED, primary ? (void *)1 : nullptr);
    return b;
}

// UHK Agent was refused config access: allow it for the connected host?
static void makeAgentCard()
{
    agentCard = makeCard();
    lv_obj_t *t = makeLabel(agentCard, &lv_font_montserrat_18, COLOR_TEXT);
    lv_label_set_text(t, LV_SYMBOL_SETTINGS " UHK Agent asks for config access");
    lv_obj_t *h = makeLabel(agentCard, &lv_font_montserrat_14, COLOR_DIM);
    lv_label_set_text(h, "Keymaps and macros, read and write,\nfrom the connected host");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *row = makeContainer(agentCard, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 18, 0);
    lv_obj_set_style_pad_top(row, 6, 0);
    makeCardButton(row, "Deny", false);
    makeCardButton(row, "Allow", true);
}

// While Agent config access is open: tap to lock it again
static lv_obj_t *makeAgentButton(lv_obj_t *parent)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_BT), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COLOR_BT), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 5, 0);
    agentLabel = makeLabel(b, &lv_font_montserrat_12, COLOR_TEXT);
    lv_obj_center(agentLabel);
    lv_obj_add_event_cb(b, agentEvent, LV_EVENT_CLICKED, nullptr); // lock
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

void ui_set_hold_progress(uint16_t permille)
{
    static int shown = -1;
    int step = permille ? max(1, permille / 4) : 0; // ~2 px steps, not a redraw per call
    if (step == shown)
        return;
    shown = step;
    if (!step) {
        lv_obj_add_flag(holdBar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_bar_set_value(holdBar, min<uint16_t>(permille, 1000), LV_ANIM_OFF);
    lv_obj_clear_flag(holdBar, LV_OBJ_FLAG_HIDDEN);
}

// ---------------- Public ----------------
void ui_begin(const UiCallbacks &cb)
{
    cbs = cb;

    // Long press = "pair new device" hold time (the touch driver is LV_Helper's)
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER)
            i->driver->long_press_time = LONG_PRESS_MS;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_pad_column(scr, 10, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = makeContainer(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, lv_pct(100));

    // Cable [switch] Bluetooth
    lv_obj_t *row = makeContainer(left, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    cableLabel = makeModeLabel(row, LV_SYMBOL_USB " Cable", false);
    modeSwitch = makeModeSwitch(row);
    btLabel    = makeModeLabel(row, LV_SYMBOL_BLUETOOTH " Bluetooth", true);

    usbLabel = makeLabel(left, &lv_font_montserrat_16, COLOR_TEXT);
    bleLabel = makeLabel(left, &lv_font_montserrat_16, COLOR_TEXT);

    slotRow = makeContainer(left, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(slotRow, lv_pct(100));
    lv_obj_set_flex_align(slotRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slotRow, 10, 0);
    slotLabel = makeLabel(slotRow, &lv_font_montserrat_14, COLOR_NEON_HI);
    pairBtn = makePairButton(slotRow);
    agentBtn = makeAgentButton(slotRow);

    statsLabel = makeLabel(left, &lv_font_montserrat_12, COLOR_DIM);
    noteLabel  = makeLabel(left, &lv_font_montserrat_14, COLOR_NOTE);

    logLabel = makeLabel(left, &lv_font_montserrat_12, COLOR_DIM);
    lv_obj_set_width(logLabel, lv_pct(100));
    lv_obj_set_flex_grow(logLabel, 1);
    lv_label_set_long_mode(logLabel, LV_LABEL_LONG_CLIP);

    makeStrip(scr);
    layoutStrip();
    makeHoldBar();
    makeAgentCard();
    makePasskeyCard();
}

void ui_update(bool btMode, const char *bootNote)
{
    BridgeStatus bs;
    bridge_get_status(bs);
    BleHidStatus ble;
    ble_hid_get_status(ble);
    char buf[96];

    // Follow mode changes made elsewhere (boot); no animation
    if (lv_obj_has_state(modeSwitch, LV_STATE_CHECKED) != btMode) {
        if (btMode)
            lv_obj_add_state(modeSwitch, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(modeSwitch, LV_STATE_CHECKED);
    }
    static int shownMode = -1; // restyle only on change (every set invalidates)
    if (btMode != shownMode) {
        shownMode = btMode;
        lv_obj_set_style_text_color(cableLabel, lv_color_hex(btMode ? COLOR_OFF : COLOR_CABLE), 0);
        lv_obj_set_style_text_color(btLabel, lv_color_hex(btMode ? COLOR_BT : COLOR_OFF), 0);
    }

    // Slot strip and pairing only make sense in Bluetooth mode
    setVisible(strip, btMode);
    setVisible(slotRow, btMode);
    if (ble.slot != shownSlot) { // changed elsewhere (boot): no animation
        shownSlot = ble.slot;
        moveBubble(shownSlot, false);
    }
    static int shownSlots = -1;
    int slotState = shownSlot;
    for (uint8_t s = 0; s < BLE_SLOTS; s++)
        slotState |= ble.slotUsed[s] << (4 + s);
    if (slotState != shownSlots) {
        shownSlots = slotState;
        for (uint8_t s = 0; s < BLE_SLOTS; s++)
            lv_obj_set_style_text_color(slotNum[s], lv_color_hex(s == shownSlot ? 0xFFFFFF
                                        : ble.slotUsed[s] ? COLOR_USED : COLOR_OFF), 0);
    }

    if (!btMode)
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " keyboard -> PC by cable");
    else if (bs.usbConnected)
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %s", bs.usbProduct[0] ? bs.usbProduct : "keyboard");
    else
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " waiting for the keyboard");
    setText(usbLabel, buf);

    if (!btMode) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " off");
    } else if (!ble.started) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " waiting for the keyboard layout");
    } else if (ble.connected) {
        int n = snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " %s, MTU %u",
                         ble.ready ? "connected" : ble.passkeyShown ? "pairing" : "authenticating...",
                         ble.mtu);
        if (ble.interval)
            snprintf(buf + n, sizeof(buf) - n, ", %u.%02u ms", ble.interval * 125 / 100,
                     ble.interval * 125 % 100);
    } else if (ble.pairingLeftMs) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " pair \"%s %u\" on the new device",
                 BLE_DEVICE_NAME, ble.slot + 1);
    } else if (ble.slotUsed[ble.slot]) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " waiting for the host");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " no host - hold BOOT to pair one");
    }
    setText(bleLabel, buf);

    bool used = ble.slotUsed[ble.slot];
    const uint8_t *h = ble.slotHost[ble.slot];
    if (ble.pairingLeftMs)
        snprintf(buf, sizeof(buf), "Slot %u: discoverable", ble.slot + 1);
    else if (used)
        snprintf(buf, sizeof(buf), "Slot %u: paired with %02X:%02X:%02X", ble.slot + 1, h[3], h[4], h[5]);
    else
        snprintf(buf, sizeof(buf), "Slot %u: free", ble.slot + 1);
    setText(slotLabel, buf);

    pairingShown = ble.pairingLeftMs > 0;
    if (pairingShown)
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " Pairing %u s - tap / BOOT to cancel",
                 (unsigned)((ble.pairingLeftMs + 999) / 1000));
    else
        snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " %s (hold)", used ? "Pair new device" : "Pair device");
    setText(pairLabel, buf);
    setVisible(pairBtn, ble.started);

    setVisible(agentBtn, bs.agentUnlockedLeftMs > 0);
    if (bs.agentUnlockedLeftMs) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS " Agent open %u min - tap to lock",
                 (unsigned)((bs.agentUnlockedLeftMs + 59999) / 60000));
        setText(agentLabel, buf);
    }
    setVisible(passkeyCard, btMode && ble.passkeyShown);
    if (ble.passkeyShown) {
        snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)(ble.passkey / 1000),
                 (unsigned long)(ble.passkey % 1000));
        setText(passkeyLabel, buf);
    }
    setVisible(agentCard, btMode && bs.agentAsking && !ble.passkeyShown);

    snprintf(buf, sizeof(buf), "fw %s %s | reports: %lu sent, %lu merged, %lu dropped", __DATE__,
             __TIME__, (unsigned long)bs.forwarded, (unsigned long)bs.merged, (unsigned long)bs.dropped);
    setText(statsLabel, buf);

    OtaStatus ota;
    ota_get_status(ota);
    char otaBuf[64];
    const char *note = bs.note ? bs.note : bootNote ? bootNote : "";
    if (ota.active && ota.total) {
        snprintf(otaBuf, sizeof(otaBuf), LV_SYMBOL_DOWNLOAD " Firmware update %lu%%  (%lu / %lu KB)",
                 (unsigned long)((uint64_t)ota.received * 100 / ota.total),
                 (unsigned long)(ota.received / 1024), (unsigned long)(ota.total / 1024));
        note = otaBuf;
    }
    setText(noteLabel, note);
    setVisible(noteLabel, note[0] != 0);

    static char logBuf[1024];
    if (log_screen_text(logBuf, sizeof(logBuf)))
        lv_label_set_text(logLabel, logBuf);
}
