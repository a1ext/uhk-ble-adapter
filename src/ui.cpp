#include "ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include "anim.h"
#include "ble_hid.h"
#include "bridge.h"
#include "cal.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "ota.h"
#include "overlay.h"
#include "stats.h"
#include "sysmon.h"

static const uint32_t COLOR_BT     = 0x0A84FF; // iOS blue
static const uint32_t COLOR_TEXT   = 0xE0E0E0;
static const uint32_t COLOR_DIM    = 0x707880;
static const uint32_t COLOR_IDLE   = 0x8E8E93; // title while no host is connected
static const uint32_t COLOR_NOTE   = 0xFFA000;
static const uint32_t COLOR_RED    = 0xFF453A;
static const uint32_t COLOR_TRACK  = 0x1C1C1E;
static const uint32_t COLOR_NEON   = 0xFF2EC4; // active slot
static const uint32_t COLOR_NEON_HI = 0xFF9BE6;
static const uint32_t COLOR_GREEN   = 0x39FF6A; // typing statistics
static const uint32_t COLOR_AMBER   = 0xFFB52E; // the slot host's name in the title

static const uint32_t COLOR_VIOLET  = 0xA23BFF; // active pad gradient

// Slot selector: a little PCB, gold pads and copper traces
static const uint32_t PCB_MASK    = 0x0B0E0C;
static const uint32_t PCB_COPPER  = 0x6A5420; // traces under the mask
static const uint32_t PCB_GOLD    = 0xD9B342;
static const uint32_t PCB_GOLD_HI = 0xF2D46A;
static const uint32_t PCB_GOLD_DK = 0xA9822A;

static const lv_coord_t STRIP_W        = 116; // slot selector column (also the tap area)
static const lv_coord_t PAD_MARGIN     = 26;  // first / last pad centre from the column ends
static const lv_coord_t PAD_HALF       = 15;  // pads are 30 x 30
static const lv_coord_t PAD_OFFSET     = 27;  // pad centre from the bus
static const lv_coord_t STRIP_EXT_DRAW = 24;  // glow outside the column
static const uint32_t   SLOT_ANIM_MS   = 750;
static const int        SPARK_END      = 420; // of 1000: the spark runs, then the pad lights up

static UiCallbacks cbs;
static lv_obj_t *titleLabel, *hostLabel, *clockLabel, *mainScr;
static lv_obj_t *usbLabel, *bleLabel, *slotRow, *slotLabel, *pairBtn, *pairLabel;
static bool pairingShown = false;
static lv_obj_t *countLabel, *statsLabel, *noteLabel, *logLabel;
static lv_obj_t *strip, *slotCell[BLE_SLOTS];
static bool     slotUsedShown[BLE_SLOTS];
static uint8_t  slotAnimFrom = 0xFF;
static int32_t  slotAnimT = 1000; // 0..1000, 1000 = settled
static lv_obj_t *holdBar;
static lv_obj_t *passkeyCard, *passkeyLabel, *agentCard, *agentBtn, *agentLabel;
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

// ---------------- Slot selector ----------------
// A little PCB: a copper bus with one gold pad per slot, branching left and
// right. Gold = a host is paired, an empty ring = free. The bus is lit from
// the via at the top down to the active slot, whose pad glows neon; switching
// sends a spark down the bus, then the new pad lights up.
static float easeInOutCubic(float x)
{
    return x < 0.5f ? 4 * x * x * x : 1 - powf(-2 * x + 2, 3) / 2;
}

static float easeOutBack(float x)
{
    const float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * powf(x - 1, 3) + c1 * powf(x - 1, 2);
}

static int slotSide(uint8_t s)
{
    return s % 2 ? 1 : -1; // slot 1 left, 2 right, ...
}

// Pad centre row of slot s, relative to the strip
static lv_coord_t slotY(uint8_t s)
{
    lv_coord_t h = lv_obj_get_height(strip);
    return PAD_MARGIN + s * (h - 2 * PAD_MARGIN) / max(1, BLE_SLOTS - 1);
}

// Where the slot's trace leaves the bus, relative to the strip
static lv_coord_t junctionY(uint8_t s)
{
    return slotY(s) - 10;
}

static void drawLine(lv_draw_ctx_t *ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2,
                     uint32_t color, lv_coord_t width, lv_opa_t opa = LV_OPA_COVER)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_point_t p1 = {x1, y1}, p2 = {x2, y2};
    lv_draw_line(ctx, &d, &p1, &p2);
}

static void setGradient(lv_draw_rect_dsc_t &d, uint32_t from, uint32_t to, lv_grad_dir_t dir)
{
    d.bg_color = lv_color_hex(from);
    d.bg_grad.dir = dir;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(from);
    d.bg_grad.stops[1].color = lv_color_hex(to);
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].frac = 255;
}

// A 45 degree jog off the bus, then straight to the pad
static void drawTrace(lv_draw_ctx_t *ctx, lv_coord_t cx, lv_coord_t y, int side, uint32_t color,
                      lv_coord_t width, lv_opa_t opa)
{
    drawLine(ctx, cx, y - 10, cx + side * 10, y, color, width, opa);
    drawLine(ctx, cx + side * 10, y, cx + side * (PAD_OFFSET - PAD_HALF), y, color, width, opa);
}

static void stripDrawEvent(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, STRIP_EXT_DRAW);
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(strip, &a);
    lv_coord_t cx = (a.x1 + a.x2) / 2, via = a.y1 + 9;
    bool animating = slotAnimT < 1000 && slotAnimFrom < BLE_SLOTS && slotAnimFrom != shownSlot;
    float spark = min(1.0f, slotAnimT / (float)SPARK_END);
    float pop = slotAnimT <= SPARK_END ? 0 : (slotAnimT - SPARK_END) / (float)(1000 - SPARK_END);

    lv_draw_rect_dsc_t d;

    // Bus, lit from the via down to the active slot; while switching, the
    // lit end runs from the old junction to the new one
    drawLine(ctx, cx, via, cx, (lv_coord_t)(a.y2 - 8), PCB_COPPER, 4);
    lv_coord_t litEnd = via;
    if (shownSlot < BLE_SLOTS) {
        float y1 = a.y1 + junctionY(shownSlot);
        if (animating) {
            float y0 = a.y1 + junctionY(slotAnimFrom);
            y1 = y0 + (y1 - y0) * easeInOutCubic(spark);
        }
        litEnd = lroundf(y1);
        drawLine(ctx, cx, via, cx, litEnd, COLOR_NEON, 10, LV_OPA_30);
        drawLine(ctx, cx, via, cx, litEnd, COLOR_NEON, 4);
    }
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(PCB_GOLD);
    lv_area_t viaPad = {(lv_coord_t)(cx - 5), (lv_coord_t)(via - 5), (lv_coord_t)(cx + 5), (lv_coord_t)(via + 5)};
    lv_draw_rect(ctx, &d, &viaPad);

    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        int side = slotSide(s);
        lv_coord_t yc = a.y1 + slotY(s), px = cx + side * PAD_OFFSET;
        // How much of the neon is on: the active pad lights up after the
        // spark, the previous one fades out while it runs
        float lit = s == shownSlot ? (animating ? min(1.0f, pop * 3) : 1)
                  : animating && s == slotAnimFrom ? 1 - spark : 0;
        drawTrace(ctx, cx, yc, side, PCB_COPPER, 3, LV_OPA_COVER);
        if (lit > 0) {
            drawTrace(ctx, cx, yc, side, COLOR_NEON, 9, (lv_opa_t)(LV_OPA_30 * lit));
            drawTrace(ctx, cx, yc, side, COLOR_NEON, 3, (lv_opa_t)(255 * lit));
        }

        lv_coord_t half = PAD_HALF;
        if (s == shownSlot && animating && pop > 0)
            half = lroundf(PAD_HALF - 3 + 3 * easeOutBack(pop)); // pops out a little
        lv_area_t pad = {(lv_coord_t)(px - half), (lv_coord_t)(yc - half), (lv_coord_t)(px + half),
                         (lv_coord_t)(yc + half)};
        lv_draw_rect_dsc_init(&d);
        d.radius = 6;
        if (slotUsedShown[s]) {
            setGradient(d, PCB_GOLD_HI, PCB_GOLD_DK, LV_GRAD_DIR_VER);
        } else {
            d.bg_color = lv_color_hex(PCB_MASK);
            d.border_color = lv_color_hex(PCB_GOLD);
            d.border_width = 2;
        }
        lv_draw_rect(ctx, &d, &pad);
        if (lit > 0) {
            lv_draw_rect_dsc_init(&d);
            d.radius = 6;
            setGradient(d, COLOR_NEON, COLOR_VIOLET, LV_GRAD_DIR_VER);
            d.bg_opa = (lv_opa_t)(255 * lit);
            d.shadow_color = lv_color_hex(COLOR_NEON);
            d.shadow_width = 22;
            d.shadow_spread = 1;
            d.shadow_opa = (lv_opa_t)(LV_OPA_80 * lit);
            lv_draw_rect(ctx, &d, &pad);
        }
        lv_draw_rect_dsc_init(&d); // drill hole
        d.radius = LV_RADIUS_CIRCLE;
        d.bg_color = lv_color_hex(PCB_MASK);
        lv_area_t hole = {(lv_coord_t)(px - 5), (lv_coord_t)(yc - 5), (lv_coord_t)(px + 5), (lv_coord_t)(yc + 5)};
        lv_draw_rect(ctx, &d, &hole);

        // Silkscreen designator on the other side of the bus
        char label[4];
        snprintf(label, sizeof(label), "S%u", s + 1);
        lv_draw_label_dsc_t t;
        lv_draw_label_dsc_init(&t);
        t.font = &lv_font_montserrat_12;
        t.color = lv_color_white();
        t.opa = s == shownSlot ? LV_OPA_COVER : LV_OPA_70;
        t.align = LV_TEXT_ALIGN_CENTER;
        lv_coord_t lx = cx - side * 22;
        lv_area_t la = {(lv_coord_t)(lx - 14), (lv_coord_t)(yc - 7), (lv_coord_t)(lx + 14), (lv_coord_t)(yc + 8)};
        lv_draw_label(ctx, &t, &la, label, nullptr);
    }

    if (animating && spark < 1) {
        lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE;
        d.bg_color = lv_color_hex(COLOR_NEON_HI);
        d.shadow_color = lv_color_hex(COLOR_NEON);
        d.shadow_width = 18;
        d.shadow_spread = 2;
        d.shadow_opa = LV_OPA_80;
        lv_area_t dot = {(lv_coord_t)(cx - 5), (lv_coord_t)(litEnd - 5), (lv_coord_t)(cx + 5), (lv_coord_t)(litEnd + 5)};
        lv_draw_rect(ctx, &d, &dot);
    }
}

static void slotAnimCb(void *, int32_t v)
{
    slotAnimT = v;
    lv_obj_invalidate(strip);
}

// Makes `slot` the highlighted one; animated: spark down the bus + pad lights up
static void showSlot(uint8_t slot, bool animate)
{
    lv_anim_del(strip, slotAnimCb);
    slotAnimFrom = shownSlot;
    shownSlot = slot;
    slotAnimT = 1000;
    if (animate && slotAnimFrom < BLE_SLOTS && slotAnimFrom != slot) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, strip);
        lv_anim_set_exec_cb(&a, slotAnimCb);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_time(&a, SLOT_ANIM_MS);
        lv_anim_start(&a);
        slotAnimT = 0;
    }
    lv_obj_invalidate(strip);
}

static void slotEvent(lv_event_t *e)
{
    uint8_t slot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (slot == shownSlot) { // the active one: its settings
        ui_open_menu();
        return;
    }
    showSlot(slot, true);
    if (cbs.onSlotRequest)
        cbs.onSlotRequest(slot);
}

static void makeStrip(lv_obj_t *parent)
{
    strip = lv_obj_create(parent);
    lv_obj_set_size(strip, STRIP_W, lv_pct(100));
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(strip, stripDrawEvent, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(strip, stripDrawEvent, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);

    // Tap targets, one row per slot (the pads are drawn by stripDrawEvent)
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        lv_obj_t *cell = lv_obj_create(strip);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cell, slotEvent, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)s);
        slotCell[s] = cell;
    }
    lv_obj_refresh_ext_draw_size(strip);
}

// Cell geometry needs the strip's final height
static void layoutStrip()
{
    lv_obj_update_layout(lv_scr_act());
    lv_coord_t h = lv_obj_get_height(strip);
    lv_coord_t pitch = (h - 2 * PAD_MARGIN) / max(1, BLE_SLOTS - 1);
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        lv_coord_t y0 = s == 0 ? 0 : slotY(s) - pitch / 2;
        lv_coord_t y1 = s == BLE_SLOTS - 1 ? h : slotY(s) + pitch / 2;
        lv_obj_set_pos(slotCell[s], 0, y0);
        lv_obj_set_size(slotCell[s], STRIP_W, y1 - y0);
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

// ---------------- Settings menu (top layer) ----------------
// A short press of BOOT, or a tap on the active slot: the active slot's host
// name (rename with the on-screen keyboard), the standby animation switch and
// its day / night brightness. A tap beside the card closes it.
static const uint8_t BRIGHT_MIN = 5;

static lv_obj_t *menuShade, *menuTitle, *hostNameLabel, *hostAddrLabel, *renameBtn;
static lv_obj_t *standbyMatrix, *animHint, *daySlider, *nightSlider, *dayValue, *nightValue;
static lv_obj_t *kbdSwitch, *kbdSlider, *kbdValue, *kbdHint;
static lv_obj_t *renamePanel, *renameTitle, *renameText, *renameKbd;
static uint8_t menuSlot;

static void setPercent(lv_obj_t *label, int32_t v)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)((v * 100 + 127) / 255));
    setText(label, buf);
}

static void setSeconds(lv_obj_t *label, int32_t v) // 0 = the keyboard's own timeout
{
    char buf[8];
    if (v)
        snprintf(buf, sizeof(buf), "%d s", (int)v);
    else
        snprintf(buf, sizeof(buf), "auto");
    setText(label, buf);
}

// The switch saves at once, a slider when it is let go
static void animSettingEvent(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && o != standbyMatrix && o != kbdSwitch) {
        if (o == kbdSlider)
            setSeconds(kbdValue, lv_slider_get_value(o));
        else
            setPercent(o == daySlider ? dayValue : nightValue, lv_slider_get_value(o));
        return;
    }
    AnimSettings s = anim_settings();
    s.kbdWake = lv_obj_has_state(kbdSwitch, LV_STATE_CHECKED);
    s.kbdIdleS = lv_slider_get_value(kbdSlider);
    uint16_t pick = lv_btnmatrix_get_selected_btn(standbyMatrix);
    if (pick <= ANIM_MODE_BOTH) {
        s.mode = pick;
        // fires on the press; the matrix itself moves the check mark only on a clean
        // release, which a finger that slides a little never delivers
        lv_btnmatrix_set_btn_ctrl(standbyMatrix, pick, LV_BTNMATRIX_CTRL_CHECKED);
    }
    s.brightness = lv_slider_get_value(daySlider);
    s.nightBrightness = lv_slider_get_value(nightSlider);
    anim_set_settings(s);
}

static void showRename(bool on)
{
    setVisible(renamePanel, on);
}

static void renameEvent(lv_event_t *e)
{
    if (lv_event_get_target(e) == renameBtn) { // open the keyboard with the current name
        BleHidStatus ble;
        ble_hid_get_status(ble);
        char buf[32];
        snprintf(buf, sizeof(buf), LV_SYMBOL_EDIT " Slot %u host", menuSlot + 1);
        lv_label_set_text(renameTitle, buf);
        lv_textarea_set_text(renameText, ble.slotName[menuSlot]);
        showRename(true);
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_READY) { // the keyboard's OK key; trimmed, "" = no name
        const char *t = lv_textarea_get_text(renameText);
        while (*t == ' ')
            t++;
        char name[BLE_HOST_NAME_MAX + 1];
        snprintf(name, sizeof(name), "%s", t);
        for (int n = strlen(name); n && name[n - 1] == ' ';)
            name[--n] = 0;
        ble_hid_set_host_name(menuSlot, name);
    }
    showRename(false); // OK or the keyboard's close key
}

static void menuShadeEvent(lv_event_t *e)
{
    if (lv_event_get_target(e) == menuShade) // beside the card
        ui_close_menu();
}

// Thin outlined pill, like the pairing button
static lv_obj_t *makePillButton(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 7, 0);
    lv_obj_set_ext_click_area(b, 8);
    return b;
}

static lv_obj_t *makeMenuRow(lv_obj_t *parent)
{
    lv_obj_t *r = makeContainer(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    return r;
}

// Name above a dim hint, taking the row's free width
static void makeMenuText(lv_obj_t *row, lv_obj_t **name, lv_obj_t **hint)
{
    lv_obj_t *col = makeContainer(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_obj_set_flex_grow(col, 1);
    *name = makeLabel(col, &lv_font_montserrat_18, COLOR_TEXT);
    *hint = makeLabel(col, &lv_font_montserrat_12, COLOR_DIM);
}

// Neon -> blue like the hold bar
static void styleNeon(lv_obj_t *o, lv_style_selector_t sel)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_NEON), sel);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(COLOR_BT), sel);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_HOR, sel);
}

static void styleKnob(lv_obj_t *o, lv_style_selector_t glow)
{
    lv_obj_set_style_bg_color(o, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_shadow_color(o, lv_color_hex(COLOR_NEON), glow);
    lv_obj_set_style_shadow_width(o, 12, glow);
    lv_obj_set_style_shadow_opa(o, LV_OPA_60, glow);
}

static lv_obj_t *makeSlider(lv_obj_t *row, const char *caption, lv_obj_t **value, int32_t min = BRIGHT_MIN,
                            int32_t max = 255)
{
    lv_obj_t *l = makeLabel(row, &lv_font_montserrat_16, COLOR_DIM);
    lv_label_set_text(l, caption);
    lv_obj_set_width(l, 56);
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_height(s, 10);
    lv_obj_set_flex_grow(s, 1);
    lv_slider_set_range(s, min, max);
    lv_obj_set_ext_click_area(s, 24);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2C2C30), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    styleNeon(s, LV_PART_INDICATOR);
    styleKnob(s, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);
    lv_obj_add_event_cb(s, animSettingEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s, animSettingEvent, LV_EVENT_RELEASED, nullptr);
    *value = makeLabel(row, &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_width(*value, 52);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
    return s;
}

static void makeMenu()
{
    // On the sys layer: under an opaque full-screen object there LVGL skips
    // drawing the screen and the top layer (it does not for the top layer), which
    // made every menu frame redraw the main screen's neon glows beneath
    menuShade = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(menuShade, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(menuShade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(menuShade, LV_OPA_70, 0);
    lv_obj_set_style_border_width(menuShade, 0, 0);
    lv_obj_set_style_radius(menuShade, 0, 0);
    lv_obj_set_style_pad_all(menuShade, 0, 0);
    lv_obj_clear_flag(menuShade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menuShade, menuShadeEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(menuShade, LV_OBJ_FLAG_HIDDEN);

    // The whole screen, scrolling down: room for finger-sized controls
    lv_obj_t *c = makeCard();
    lv_obj_set_parent(c, menuShade);
    lv_obj_set_size(c, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(c, 20, 0);
    lv_obj_set_style_pad_ver(c, 12, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_NEON), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(c, 4, LV_PART_SCROLLBAR);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *r = makeMenuRow(c);
    menuTitle = makeLabel(r, &lv_font_montserrat_20, COLOR_NEON_HI);
    lv_obj_t *close = makePillButton(r, COLOR_DIM);
    lv_obj_set_style_pad_hor(close, 18, 0);
    lv_obj_set_style_pad_ver(close, 10, 0);
    lv_obj_t *l = makeLabel(close, &lv_font_montserrat_18, COLOR_TEXT);
    lv_label_set_text(l, LV_SYMBOL_CLOSE);
    lv_obj_add_event_cb(close, [](lv_event_t *) { ui_close_menu(); }, LV_EVENT_CLICKED, nullptr);

    r = makeMenuRow(c);
    makeMenuText(r, &hostNameLabel, &hostAddrLabel);
    renameBtn = makePillButton(r, COLOR_NEON);
    lv_obj_set_style_pad_hor(renameBtn, 22, 0);
    lv_obj_set_style_pad_ver(renameBtn, 12, 0);
    l = makeLabel(renameBtn, &lv_font_montserrat_18, COLOR_NEON_HI);
    lv_label_set_text(l, LV_SYMBOL_EDIT " Rename");
    lv_obj_add_event_cb(renameBtn, renameEvent, LV_EVENT_CLICKED, nullptr);

    r = makeMenuRow(c);
    lv_obj_t *animName;
    makeMenuText(r, &animName, &animHint);
    lv_label_set_text(animName, "Standby screen");
    // Off / the animation / the dashboard / both in turns (anim.h)
    static const char *standbyMap[] = {"Off", "Anim", "Dash", "Both", ""};
    standbyMatrix = lv_btnmatrix_create(c);          // a row of its own, finger-sized
    lv_btnmatrix_set_map(standbyMatrix, standbyMap);
    lv_btnmatrix_set_btn_ctrl_all(standbyMatrix, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(standbyMatrix, true);
    lv_obj_set_size(standbyMatrix, lv_pct(100), 60);
    lv_obj_add_flag(standbyMatrix, LV_OBJ_FLAG_PRESS_LOCK); // a finger that slides keeps its press
    lv_obj_set_style_bg_opa(standbyMatrix, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(standbyMatrix, 0, 0);
    lv_obj_set_style_pad_all(standbyMatrix, 0, 0);
    lv_obj_set_style_pad_gap(standbyMatrix, 10, 0);
    lv_obj_set_style_text_font(standbyMatrix, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_text_color(standbyMatrix, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(standbyMatrix, lv_color_hex(0x2C2C30), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(standbyMatrix, LV_OPA_COVER, LV_PART_ITEMS);
    styleNeon(standbyMatrix, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(standbyMatrix, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(standbyMatrix, LV_RADIUS_CIRCLE, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(standbyMatrix, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(standbyMatrix, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(standbyMatrix, animSettingEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    l = makeLabel(c, &lv_font_montserrat_12, COLOR_DIM);
    lv_label_set_text(l, "Brightness of the standby screen and the live spectrum");
    lv_obj_set_style_pad_bottom(l, -4, 0);
    r = makeMenuRow(c);
    lv_obj_set_style_pad_column(r, 16, 0);
    daySlider = makeSlider(r, "Day", &dayValue);
    r = makeMenuRow(c);
    lv_obj_set_style_pad_column(r, 16, 0);
    nightSlider = makeSlider(r, "Night", &nightValue);

    // The keyboard wakes the screen (anim.h kbdWake / kbdIdleS)
    r = makeMenuRow(c);
    lv_obj_t *kbdName;
    makeMenuText(r, &kbdName, &kbdHint);
    lv_label_set_text(kbdName, "Keyboard wakes the screen");
    lv_label_set_text(kbdHint, "typing shows the standby screen, idle turns it off");
    kbdSwitch = lv_switch_create(r);
    lv_obj_set_size(kbdSwitch, 64, 34);
    lv_obj_set_style_bg_color(kbdSwitch, lv_color_hex(0x2C2C30), LV_PART_MAIN);
    styleNeon(kbdSwitch, LV_PART_INDICATOR | LV_STATE_CHECKED);
    styleKnob(kbdSwitch, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(kbdSwitch, 14);
    lv_obj_add_event_cb(kbdSwitch, animSettingEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    r = makeMenuRow(c);
    lv_obj_set_style_pad_column(r, 16, 0);
    kbdSlider = makeSlider(r, "Idle", &kbdValue, 0, 600); // 0 = the keyboard's own backlight timeout
}

// Full screen: the name above the keyboard (OK saves, the close key cancels)
static void makeRenamePanel()
{
    renamePanel = lv_obj_create(lv_layer_sys()); // opaque, full screen: nothing beneath is drawn
    lv_obj_set_size(renamePanel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(renamePanel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(renamePanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(renamePanel, 0, 0);
    lv_obj_set_style_radius(renamePanel, 0, 0);
    lv_obj_set_style_pad_all(renamePanel, 6, 0);
    lv_obj_set_style_pad_row(renamePanel, 6, 0);
    lv_obj_set_flex_flow(renamePanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(renamePanel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *r = makeMenuRow(renamePanel);
    lv_obj_set_style_pad_left(r, 6, 0);
    renameTitle = makeLabel(r, &lv_font_montserrat_16, COLOR_NEON_HI);
    renameText = lv_textarea_create(r);
    lv_obj_set_flex_grow(renameText, 1);
    lv_textarea_set_one_line(renameText, true);
    lv_textarea_set_max_length(renameText, BLE_HOST_NAME_MAX);
    lv_textarea_set_placeholder_text(renameText, "e.g. MacBook");
    lv_obj_set_style_text_font(renameText, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(renameText, lv_color_white(), 0);
    lv_obj_set_style_text_color(renameText, lv_color_hex(COLOR_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(renameText, lv_color_hex(0x111114), 0);
    lv_obj_set_style_border_width(renameText, 1, 0);
    lv_obj_set_style_border_color(renameText, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_border_color(renameText, lv_color_hex(COLOR_NEON), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(renameText, 10, 0);
    lv_obj_set_style_pad_ver(renameText, 6, 0);
    lv_obj_set_style_shadow_color(renameText, lv_color_hex(COLOR_NEON), 0);
    lv_obj_set_style_shadow_width(renameText, 16, 0);
    lv_obj_set_style_shadow_opa(renameText, LV_OPA_30, 0);
    lv_obj_set_style_border_color(renameText, lv_color_hex(COLOR_NEON), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_state(renameText, LV_STATE_FOCUSED); // the cursor shows and blinks

    renameKbd = lv_keyboard_create(renamePanel);
    lv_obj_set_width(renameKbd, lv_pct(100));
    lv_obj_set_flex_grow(renameKbd, 1);
    lv_keyboard_set_textarea(renameKbd, renameText);
    lv_obj_set_style_bg_opa(renameKbd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(renameKbd, 0, 0);
    lv_obj_set_style_pad_all(renameKbd, 0, 0);
    lv_obj_set_style_pad_gap(renameKbd, 5, 0);
    lv_obj_set_style_text_font(renameKbd, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_text_color(renameKbd, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(renameKbd, lv_color_hex(0x1C1C1F), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(renameKbd, lv_color_hex(0x2E2E34), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(renameKbd, lv_color_hex(COLOR_NEON_HI), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(renameKbd, lv_color_hex(COLOR_NEON), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(renameKbd, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(renameKbd, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(renameKbd, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(renameKbd, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(renameKbd, renameEvent, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(renameKbd, renameEvent, LV_EVENT_CANCEL, nullptr);

    lv_obj_add_flag(renamePanel, LV_OBJ_FLAG_HIDDEN);
}

static void updateMenu(const BleHidStatus &ble)
{
    char buf[64];
    snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS "  Settings - %s %u", BLE_DEVICE_NAME, menuSlot + 1);
    setText(menuTitle, buf);
    bool used = ble.slotUsed[menuSlot];
    const char *name = ble.slotName[menuSlot];
    const uint8_t *h = ble.slotHost[menuSlot];
    setText(hostNameLabel, !used ? "No host paired" : name[0] ? name : "Unnamed host");
    static uint32_t nameColor = 0;
    uint32_t color = used && name[0] ? 0xFFFFFF : COLOR_DIM;
    if (color != nameColor) {
        nameColor = color;
        lv_obj_set_style_text_color(hostNameLabel, lv_color_hex(color), 0);
    }
    if (used)
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", h[0], h[1], h[2], h[3], h[4], h[5]);
    else
        snprintf(buf, sizeof(buf), "hold BOOT or Pair to pair one");
    setText(hostAddrLabel, buf);
    setVisible(renameBtn, used);
    if (!used)
        showRename(false); // the host went away meanwhile
    uint8_t mode = anim_settings().mode;
    char kbd[64];
    uint16_t fromKbd = bridge_keyboard_backlight_timeout_s();
    if (fromKbd)
        snprintf(kbd, sizeof(kbd), "typing shows the standby screen; backlight fades after %u s", fromKbd);
    else
        snprintf(kbd, sizeof(kbd), "typing shows the standby screen; auto = 60 s (timeout not read)");
    setText(kbdHint, kbd);
    setText(animHint, mode == ANIM_MODE_OFF         ? "dims, then switches off"
                      : mode == ANIM_MODE_DASHBOARD ? "clock, meetings, PC load"
                      : !anim_available()           ? "no animation uploaded yet"
                      : mode == ANIM_MODE_BOTH      ? "dashboard and animation in turns"
                                                    : "animation when idle");
}

void ui_open_menu()
{
    BleHidStatus ble;
    ble_hid_get_status(ble);
    menuSlot = ble.slot;
    const AnimSettings &s = anim_settings();
    lv_btnmatrix_set_btn_ctrl(standbyMatrix, min<uint8_t>(s.mode, ANIM_MODE_BOTH), LV_BTNMATRIX_CTRL_CHECKED);
    lv_slider_set_value(daySlider, s.brightness, LV_ANIM_OFF);
    lv_slider_set_value(nightSlider, s.nightBrightness, LV_ANIM_OFF);
    setPercent(dayValue, lv_slider_get_value(daySlider));
    setPercent(nightValue, lv_slider_get_value(nightSlider));
    if (s.kbdWake)
        lv_obj_add_state(kbdSwitch, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(kbdSwitch, LV_STATE_CHECKED);
    lv_slider_set_value(kbdSlider, s.kbdIdleS, LV_ANIM_OFF);
    setSeconds(kbdValue, lv_slider_get_value(kbdSlider));
    updateMenu(ble);
    showRename(false);
    setVisible(menuShade, true);
}

void ui_close_menu()
{
    showRename(false);
    setVisible(menuShade, false);
}

bool ui_menu_open()
{
    return !lv_obj_has_flag(menuShade, LV_OBJ_FLAG_HIDDEN);
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

// 1234567 -> "1 234 567"
static void groupDigits(uint64_t v, char *out, size_t size)
{
    char raw[24];
    int n = snprintf(raw, sizeof(raw), "%llu", (unsigned long long)v);
    size_t pos = 0;
    for (int i = 0; i < n && pos + 2 < size; i++) {
        if (i && (n - i) % 3 == 0)
            out[pos++] = ' ';
        out[pos++] = raw[i];
    }
    out[pos] = 0;
}

// ---------------- Public ----------------
// ---------------- Meeting reminder (top layer) ----------------
// CAL_REMIND_MIN before a meeting: when, what, its time and an "X" that
// dismisses it. A confirmed meeting gets a solid card, a tentative one a
// translucent card. On the LVGL screens it is an ordinary object; while the
// standby animation / live spectrum own the panel a snapshot of it goes to the
// overlay compositor (overlay.h), blended into whatever they draw.
static const lv_coord_t REMIND_X = 10, REMIND_Y = 4, REMIND_W = 394, REMIND_H = 40; // over the title row
static lv_obj_t    *remindCard, *remindWhen, *remindTitle, *remindTime, *remindClose;
static uint16_t     remindId;
static bool         remindChanged, overlayOn;
static uint8_t     *remindSnap;
static uint32_t     remindSnapSize;
static lv_img_dsc_t remindDsc;

static void remindCloseEvent(lv_event_t *)
{
    cal_dismiss(remindId);
}

static void makeReminderCard()
{
    remindCard = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(remindCard, REMIND_X, REMIND_Y);
    lv_obj_set_size(remindCard, REMIND_W, REMIND_H);
    lv_obj_set_style_bg_color(remindCard, lv_color_hex(0x121216), 0);
    lv_obj_set_style_border_width(remindCard, 1, 0);
    lv_obj_set_style_radius(remindCard, 14, 0);
    lv_obj_set_style_shadow_width(remindCard, 0, 0); // nothing outside its box: the snapshot is the card
    lv_obj_set_style_pad_hor(remindCard, 12, 0);
    lv_obj_set_style_pad_ver(remindCard, 0, 0);
    lv_obj_set_style_pad_column(remindCard, 8, 0);
    lv_obj_set_flex_flow(remindCard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(remindCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(remindCard, LV_OBJ_FLAG_SCROLLABLE);
    remindWhen = makeLabel(remindCard, &lv_font_montserrat_16, COLOR_AMBER);
    remindTitle = makeLabel(remindCard, &lv_font_montserrat_16, 0xFFFFFF);
    lv_obj_set_flex_grow(remindTitle, 1);
    lv_label_set_long_mode(remindTitle, LV_LABEL_LONG_DOT);
    remindTime = makeLabel(remindCard, &lv_font_montserrat_12, COLOR_DIM);
    remindClose = lv_btn_create(remindCard);
    lv_obj_set_size(remindClose, 30, 30);
    lv_obj_set_style_bg_color(remindClose, lv_color_hex(0x2C2C30), 0);
    lv_obj_set_style_bg_opa(remindClose, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(remindClose, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_radius(remindClose, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(remindClose, 0, 0);
    lv_obj_set_style_pad_all(remindClose, 0, 0);
    lv_obj_set_ext_click_area(remindClose, 8);
    lv_obj_t *x = makeLabel(remindClose, &lv_font_montserrat_14, COLOR_TEXT);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);
    lv_obj_add_event_cb(remindClose, remindCloseEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(remindCard, LV_OBJ_FLAG_HIDDEN);
}

// "15:00" of the moment `minutes` from now by the adapter's clock; "" without one
static void clockPlus(int32_t minutes, char *out, size_t size)
{
    uint8_t h, m, s;
    if (!anim_clock_now(h, m, s)) {
        out[0] = 0;
        return;
    }
    int32_t t = ((h * 60 + m + minutes) % 1440 + 1440) % 1440;
    snprintf(out, size, "%02ld:%02ld", (long)(t / 60), (long)(t % 60));
}

static void updateReminder()
{
    static uint16_t shownId = 0xFFFF;
    static uint8_t  shownFlags = 0xFF;
    CalEvent e;
    bool show = cal_reminder(e);
    if (show) {
        char when[24], t0[8], t1[8], range[24];
        int32_t m = cal_minutes_to(e);
        if (m > 0)
            snprintf(when, sizeof(when), "in %ld min", (long)m);
        else
            snprintf(when, sizeof(when), "now");
        clockPlus(m, t0, sizeof(t0));
        clockPlus(m + e.durMin, t1, sizeof(t1));
        if (t0[0])
            snprintf(range, sizeof(range), "%s - %s", t0, t1);
        else
            snprintf(range, sizeof(range), "%u min", e.durMin);
        remindChanged |= strcmp(lv_label_get_text(remindWhen), when) || strcmp(lv_label_get_text(remindTitle), e.title)
                      || strcmp(lv_label_get_text(remindTime), range) || e.id != shownId || e.flags != shownFlags;
        setText(remindWhen, when);
        setText(remindTitle, e.title);
        setText(remindTime, range);
        if (e.flags != shownFlags) {
            bool solid = e.flags & CAL_CONFIRMED;
            lv_obj_set_style_bg_opa(remindCard, solid ? LV_OPA_COVER : LV_OPA_60, 0);
            lv_obj_set_style_border_color(remindCard, lv_color_hex(solid ? COLOR_AMBER : COLOR_DIM), 0);
            lv_obj_set_style_border_opa(remindCard, solid ? LV_OPA_COVER : LV_OPA_60, 0);
        }
        shownId = remindId = e.id;
        shownFlags = e.flags;
    }
    if (show == lv_obj_has_flag(remindCard, LV_OBJ_FLAG_HIDDEN))
        remindChanged = true;
    setVisible(remindCard, show);
}

bool ui_reminder_loop(bool panelTaken)
{
    updateReminder();
    bool want = panelTaken && !lv_obj_has_flag(remindCard, LV_OBJ_FLAG_HIDDEN);
    if (!want) {
        if (!overlayOn)
            return false;
        uint16_t x, y, w, h;
        overlay_rect(x, y, w, h);
        overlay_clear();
        overlayOn = false;
        display_fill(x, y, w, h); // the owner of the panel repaints its part
        return true;
    }
    if (overlayOn && !remindChanged)
        return false;
    lv_obj_update_layout(remindCard);
    uint32_t need = lv_snapshot_buf_size_needed(remindCard, LV_IMG_CF_TRUE_COLOR_ALPHA);
    if (need > remindSnapSize) {
        free(remindSnap);
#ifdef ARDUINO
        remindSnap = (uint8_t *)ps_malloc(need);
#else
        remindSnap = (uint8_t *)malloc(need);
#endif
        remindSnapSize = remindSnap ? need : 0;
    }
    if (!remindSnap
        || lv_snapshot_take_to_buf(remindCard, LV_IMG_CF_TRUE_COLOR_ALPHA, &remindDsc, remindSnap, need) != LV_RES_OK)
        return false;
    lv_area_t a;
    lv_obj_get_coords(remindCard, &a);
    lv_coord_t ext = _lv_obj_get_ext_draw_size(remindCard);
    overlay_set(a.x1 - ext, a.y1 - ext, remindDsc.header.w, remindDsc.header.h, remindSnap);
    overlayOn = true;
    remindChanged = false;
    display_overlay_show();
    return true;
}

bool ui_reminder_touch(int16_t x, int16_t y)
{
    if (lv_obj_has_flag(remindCard, LV_OBJ_FLAG_HIDDEN))
        return false;
    lv_area_t a;
    lv_obj_get_coords(remindClose, &a);
    const lv_coord_t m = 12; // fingers
    if (x < a.x1 - m || x > a.x2 + m || y < a.y1 - m || y > a.y2 + m)
        return false;
    cal_dismiss(remindId);
    return true;
}

// ---------------- Dashboard (standby screen) ----------------
// The clock and the date along the top edge, the next meeting under them
// (the reminder card moves down over that line while it is up, so the clock
// stays readable), RAM and VRAM as two ring gauges on the right, and CPU /
// GPU / network / disk in four tiles along the bottom, each with two minutes
// of history. Its own LVGL screen, loaded instead of the main one.
static const uint8_t    DASH_TILES = 4, DASH_RINGS = 2;
static const uint8_t    DASH_POINTS = 120, DASH_ACC = 2; // 1 px per point, 2 samples (1 s) averaged into one
static const lv_coord_t TILE_Y = 118, TILE_W = 128, TILE_H = 116, TILE_GAP = 6, TILE_X0 = 3;
static const lv_coord_t RING_BOX = 80, RING_R = 36, RING_W = 8, RING_Y = 38; // outer radius / arc width
static const lv_coord_t NEXT_Y = 74, NEXT_W = 352;                           // the meeting line, left of the rings
static const uint16_t   RING_A0 = 150, RING_SWEEP = 240;    // from bottom-left clockwise, the gap at the bottom
static lv_obj_t *dashScr, *dashClock, *dashDate, *dashNext;
static lv_obj_t *dashValues[DASH_TILES], *dashTemps[DASH_TILES], *dashCharts[DASH_TILES];
static lv_obj_t *dashRings[DASH_RINGS], *dashRingValues[DASH_RINGS];
static lv_chart_series_t *dashSeries[DASH_TILES];
static lv_coord_t dashHist[DASH_TILES][DASH_POINTS];
static uint8_t    dashRingPct[DASH_RINGS] = {0xFF, 0xFF};
static bool       dashShown, dashLive;
static const char *const dashNames[DASH_TILES] = {"CPU", "GPU", "NET", "DISK"};
static const uint32_t    dashColors[DASH_TILES] = {COLOR_NEON, 0x2EC4FF, COLOR_GREEN, COLOR_AMBER};
static const char *const ringNames[DASH_RINGS] = {"RAM", "VRAM"};
static const uint32_t    ringColors[DASH_RINGS][2] = {{COLOR_VIOLET, COLOR_NEON}, {0x2EC4FF, 0x39FFC8}}; // start → end of the ring

// A ring gauge: a dark track, the filled part a gradient from the ring's
// first colour at the start to its second at the end (the fill reveals it)
// with a soft glow, drawn as overlapping arcs from each step to the end
// One masked, gradient-filled arc: outer radius `ro`, inner `ri`, from `a0` for `sweep`
// degrees, horizontal gradient c0 -> c1 over the ring's box. The angle mask and the
// two radius masks give anti-aliased edges all around (the stacked lv_draw_arc
// segments used before showed as steps on the panel). `rd` must be static storage of
// its own per ring and layer: LVGL's gradient cache is keyed by the descriptor's
// ADDRESS and the size, so a stack descriptor served ring 2 ring 1's colours
static void ringArc(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *rd, lv_point_t c, lv_coord_t ro, lv_coord_t ri,
                    uint16_t a0, uint16_t sweep, lv_color_t c0, lv_color_t c1, lv_opa_t opa)
{
    lv_area_t outer = {(lv_coord_t)(c.x - ro), (lv_coord_t)(c.y - ro), (lv_coord_t)(c.x + ro - 1), (lv_coord_t)(c.y + ro - 1)};
    lv_area_t inner = {(lv_coord_t)(c.x - ri), (lv_coord_t)(c.y - ri), (lv_coord_t)(c.x + ri - 1), (lv_coord_t)(c.y + ri - 1)};
    lv_draw_mask_angle_param_t am;
    lv_draw_mask_radius_param_t mo, mi;
    lv_draw_mask_angle_init(&am, c.x, c.y, a0, a0 + sweep);
    lv_draw_mask_radius_init(&mo, &outer, LV_RADIUS_CIRCLE, false);
    lv_draw_mask_radius_init(&mi, &inner, LV_RADIUS_CIRCLE, true);
    int16_t ida = lv_draw_mask_add(&am, nullptr), ido = lv_draw_mask_add(&mo, nullptr), idi = lv_draw_mask_add(&mi, nullptr);
    lv_draw_rect_dsc_init(rd);
    rd->bg_opa = opa;
    rd->bg_grad.dir = LV_GRAD_DIR_HOR;
    rd->bg_grad.stops_count = 2;
    rd->bg_grad.stops[0].color = c0;
    rd->bg_grad.stops[0].frac = 0;
    rd->bg_grad.stops[1].color = c1;
    rd->bg_grad.stops[1].frac = 255;
    lv_draw_rect(ctx, rd, &outer);
    lv_draw_mask_remove_id(idi);
    lv_draw_mask_remove_id(ido);
    lv_draw_mask_remove_id(ida);
    lv_draw_mask_free_param(&mi);
    lv_draw_mask_free_param(&mo);
    lv_draw_mask_free_param(&am);
}

// A round cap of diameter `w` centred on the arc's centre line at angle `ang`, kept to
// the half outside the arc (angle mask [ang, ang + 90] or [ang - 90, ang]) so a
// translucent fill does not double up where cap and arc would overlap
static void ringCap(lv_draw_ctx_t *ctx, lv_point_t c, lv_coord_t rmid, lv_coord_t w, uint16_t ang, bool after,
                    lv_color_t color, lv_opa_t opa)
{
    lv_coord_t x = c.x + (rmid * lv_trigo_cos(ang) >> LV_TRIGO_SHIFT);
    lv_coord_t y = c.y + (rmid * lv_trigo_sin(ang) >> LV_TRIGO_SHIFT);
    lv_coord_t h = w / 2;
    lv_area_t a = {(lv_coord_t)(x - h), (lv_coord_t)(y - h), (lv_coord_t)(x + h - 1), (lv_coord_t)(y + h - 1)};
    lv_draw_mask_angle_param_t am;
    lv_draw_mask_angle_init(&am, c.x, c.y, after ? ang : (ang + 270) % 360, after ? ang + 90 : ang);
    int16_t id = lv_draw_mask_add(&am, nullptr);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_color = color;
    rd.bg_opa = opa;
    lv_draw_rect(ctx, &rd, &a);
    lv_draw_mask_remove_id(id);
    lv_draw_mask_free_param(&am);
}

static void ringDrawEvent(lv_event_t *e)
{
    uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_point_t c = {(lv_coord_t)((a.x1 + a.x2 + 1) / 2), (lv_coord_t)((a.y1 + a.y2 + 1) / 2)};

    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.width = RING_W;
    d.rounded = 1;
    d.color = lv_color_hex(0x24242A); // track, a touch brighter than COLOR_TRACK on pure black
    lv_draw_arc(ctx, &d, &c, RING_R, RING_A0, RING_A0 + RING_SWEEP);

    uint8_t pct = dashRingPct[i];
    if (!dashLive || pct == 0xFF)
        return;
    uint16_t sweep = (uint16_t)(RING_SWEEP * (uint32_t)min<uint8_t>(pct, 100) / 100);
    if (sweep < 2)
        sweep = 2;
    lv_color_t c0 = lv_color_hex(ringColors[i][0]), c1 = lv_color_hex(ringColors[i][1]);
    // The gradient runs left to right across the box; the colour the fill reaches at
    // its end is read off the same ramp for the end cap
    lv_coord_t xEnd = (RING_R * lv_trigo_cos(RING_A0 + sweep) >> LV_TRIGO_SHIFT);
    lv_color_t cEnd = lv_color_mix(c1, c0, (uint8_t)((xEnd + RING_R) * 255 / (2 * RING_R)));
    const lv_opa_t FILL = LV_OPA_80, GLOW = LV_OPA_20;
    const uint16_t end = RING_A0 + sweep;
    // glow: wider and fainter, running a little past both ends like the caps
    static lv_draw_rect_dsc_t ringDsc[DASH_RINGS][2]; // see ringArc()
    ringArc(ctx, &ringDsc[i][0], c, RING_R + 4, RING_R - RING_W - 4, RING_A0 - 6, sweep + 12, c0, c1, GLOW);
    // the fill with its round caps
    ringArc(ctx, &ringDsc[i][1], c, RING_R, RING_R - RING_W, RING_A0, sweep, c0, c1, FILL);
    ringCap(ctx, c, RING_R - RING_W / 2, RING_W, RING_A0, false, c0, FILL);
    ringCap(ctx, c, RING_R - RING_W / 2, RING_W, end % 360, true, cEnd, FILL);
}

static void makeDashboard()
{
    dashScr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(dashScr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(dashScr, 0, 0);
    lv_obj_clear_flag(dashScr, LV_OBJ_FLAG_SCROLLABLE);

    dashClock = makeLabel(dashScr, &lv_font_montserrat_48, COLOR_TEXT);
    lv_obj_set_pos(dashClock, 12, 2);
    dashDate = makeLabel(dashScr, &lv_font_montserrat_20, COLOR_DIM);
    lv_obj_set_pos(dashDate, 148, 27); // on the clock's baseline, up to the rings
    lv_obj_set_size(dashDate, 220, 26); // a fixed height: too long = dots, not a second line
    lv_label_set_long_mode(dashDate, LV_LABEL_LONG_DOT);
    dashNext = makeLabel(dashScr, &lv_font_montserrat_16, COLOR_AMBER);
    lv_obj_set_pos(dashNext, 14, NEXT_Y);
    lv_obj_set_width(dashNext, NEXT_W);
    lv_label_set_long_mode(dashNext, LV_LABEL_LONG_DOT);

    for (uint8_t i = 0; i < DASH_RINGS; i++) {
        lv_obj_t *r = dashRings[i] = lv_obj_create(dashScr);
        lv_obj_set_pos(r, lv_disp_get_hor_res(nullptr) - RING_BOX - (DASH_RINGS - 1 - i) * (RING_BOX + 4), RING_Y);
        lv_obj_set_size(r, RING_BOX, RING_BOX);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, ringDrawEvent, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)i);
        dashRingValues[i] = makeLabel(r, &lv_font_montserrat_18, COLOR_DIM);
        lv_label_set_text(dashRingValues[i], "-");
        lv_obj_align(dashRingValues[i], LV_ALIGN_CENTER, 0, -2);
        lv_obj_t *name = makeLabel(r, &lv_font_montserrat_12, COLOR_DIM);
        lv_label_set_text(name, ringNames[i]);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, 27); // in the gap at the bottom of the ring
    }

    for (uint8_t i = 0; i < DASH_TILES; i++) {
        lv_obj_t *t = lv_obj_create(dashScr);
        lv_obj_set_pos(t, TILE_X0 + i * (TILE_W + TILE_GAP), TILE_Y);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x101014), 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_pad_all(t, 6, 0);
        lv_obj_set_style_pad_hor(t, 4, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *name = makeLabel(t, &lv_font_montserrat_12, COLOR_DIM);
        lv_label_set_text(name, dashNames[i]);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 2, 3);
        dashValues[i] = makeLabel(t, &lv_font_montserrat_16, dashColors[i]);
        lv_label_set_text(dashValues[i], "-");
        lv_obj_align(dashValues[i], LV_ALIGN_TOP_RIGHT, -2, 0);
        dashTemps[i] = makeLabel(t, &lv_font_montserrat_12, COLOR_DIM); // CPU / GPU temperature
        lv_obj_align(dashTemps[i], LV_ALIGN_TOP_RIGHT, -2, 19);
        lv_obj_t *ch = dashCharts[i] = lv_chart_create(t);
        lv_obj_set_size(ch, DASH_POINTS, TILE_H - 46);
        lv_obj_align(ch, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(ch, DASH_POINTS);
        lv_chart_set_div_line_count(ch, 0, 0);
        lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ch, 0, 0);
        lv_obj_set_style_pad_all(ch, 0, 0);
        lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
        lv_obj_set_style_size(ch, 0, LV_PART_INDICATOR);
        dashSeries[i] = lv_chart_add_series(ch, lv_color_hex(dashColors[i]), LV_CHART_AXIS_PRIMARY_Y);
        for (uint8_t k = 0; k < DASH_POINTS; k++)
            dashHist[i][k] = LV_CHART_POINT_NONE;
        lv_chart_set_ext_y_array(ch, dashSeries[i], dashHist[i]);
    }
}

// Newest value at the right; the range of a rate tile follows its history
static void pushPoint(uint8_t i, lv_coord_t v, bool autoRange)
{
    memmove(dashHist[i], dashHist[i] + 1, sizeof(lv_coord_t) * (DASH_POINTS - 1));
    dashHist[i][DASH_POINTS - 1] = v;
    if (autoRange) {
        lv_coord_t top = 100;
        for (uint8_t k = 0; k < DASH_POINTS; k++)
            if (dashHist[i][k] != LV_CHART_POINT_NONE && dashHist[i][k] > top)
                top = dashHist[i][k];
        lv_chart_set_range(dashCharts[i], LV_CHART_AXIS_PRIMARY_Y, 0, top);
    }
    lv_chart_refresh(dashCharts[i]);
}

// DASH_ACC samples are averaged into one chart point (n/a samples left out)
static void addSample(uint8_t i, uint32_t v, bool valid, bool autoRange, bool flush)
{
    static uint32_t sum[DASH_TILES];
    static uint8_t  n[DASH_TILES];
    if (valid) {
        sum[i] += v;
        n[i]++;
    }
    if (!flush)
        return;
    pushPoint(i, n[i] ? (lv_coord_t)(sum[i] / n[i]) : LV_CHART_POINT_NONE, autoRange);
    sum[i] = 0;
    n[i] = 0;
}

static void percentText(uint8_t v, char *out, size_t size)
{
    if (v == 0xFF)
        snprintf(out, size, "-");
    else
        snprintf(out, size, "%u%%", v);
}

static void tempText(uint8_t t, char *out, size_t size)
{
    if (t == 0xFF)
        out[0] = 0;
    else
        snprintf(out, size, "%u\xC2\xB0", t);
}

static void rateText(uint32_t kb, char *out, size_t size) // KB/s
{
    if (kb >= 10240)
        snprintf(out, size, "%luM", (unsigned long)(kb / 1024));
    else if (kb >= 1024)
        snprintf(out, size, "%lu.%luM", (unsigned long)(kb / 1024), (unsigned long)(kb % 1024 * 10 / 1024));
    else
        snprintf(out, size, "%luK", (unsigned long)kb);
}

static void updateDashboard()
{
    char buf[64];
    uint8_t h, m, s;
    if (anim_clock_now(h, m, s))
        snprintf(buf, sizeof(buf), "%02u:%02u", h, m);
    else
        snprintf(buf, sizeof(buf), "--:--");
    setText(dashClock, buf);

    static uint32_t dateReadMs = 0;
    static char     date[40];
    if (!dateReadMs || millis() - dateReadMs > 30000) { // the RTC sits on I2C: not every tick
        dateReadMs = millis();
        uint16_t y;
        uint8_t  mo, d;
        if (display_rtc_read_date(y, mo, d)) {
            static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
            static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4}; // Sakamoto's day of the week
            int yy = y - (mo < 3);
            int wd = (yy + yy / 4 - yy / 100 + yy / 400 + t[mo - 1] + d) % 7;
            snprintf(date, sizeof(date), "%s, %u %s", days[wd], d, months[mo - 1]);
        } else {
            date[0] = 0;
        }
    }
    setText(dashDate, date);

    CalEvent e;
    bool remind = !lv_obj_has_flag(remindCard, LV_OBJ_FLAG_HIDDEN);
    buf[0] = 0;
    if (cal_next(e)) {
        int32_t mins = cal_minutes_to(e);
        char t0[8];
        clockPlus(mins, t0, sizeof(t0));
        if (mins > 0)
            snprintf(buf, sizeof(buf), LV_SYMBOL_BELL "  %s%s%s  -  in %ld min", t0, t0[0] ? "  " : "", e.title, (long)mins);
        else
            snprintf(buf, sizeof(buf), LV_SYMBOL_BELL "  now: %s", e.title);
    } else if (cal_has_data()) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BELL "  no more meetings");
    }
    setText(dashNext, buf);
    setVisible(dashNext, !remind);

    SysSample sm;
    if (sysmon_take(sm)) {
        static uint8_t accCount = 0;
        bool flush = ++accCount >= DASH_ACC;
        if (flush)
            accCount = 0;
        char v[16];
        const uint8_t pct[2] = {sm.cpu, sm.gpu};
        for (uint8_t i = 0; i < 2; i++) {
            percentText(pct[i], v, sizeof(v));
            setText(dashValues[i], v);
            addSample(i, pct[i], pct[i] != 0xFF, false, flush);
        }
        tempText(sm.cpuTemp, v, sizeof(v));
        setText(dashTemps[0], v);
        tempText(sm.gpuTemp, v, sizeof(v));
        setText(dashTemps[1], v);
        uint32_t net = (sm.netRx == 0xFFFF ? 0 : sm.netRx) + (sm.netTx == 0xFFFF ? 0 : sm.netTx);
        uint32_t disk = ((sm.diskR == 0xFFFF ? 0 : sm.diskR) + (sm.diskW == 0xFFFF ? 0 : sm.diskW)) * 100UL;
        rateText(net, v, sizeof(v));
        setText(dashValues[2], v);
        rateText(disk, v, sizeof(v));
        setText(dashValues[3], v);
        addSample(2, min<uint32_t>(net, 30000), true, true, flush);
        addSample(3, min<uint32_t>(disk, 30000), true, true, flush);

        const uint8_t ring[DASH_RINGS] = {sm.ram, sm.vram};
        for (uint8_t i = 0; i < DASH_RINGS; i++) {
            percentText(ring[i], v, sizeof(v));
            setText(dashRingValues[i], v);
            if (ring[i] != dashRingPct[i]) {
                dashRingPct[i] = ring[i];
                lv_obj_invalidate(dashRings[i]);
            }
        }
    }
    bool live = sysmon_streaming();
    if (live != dashLive) {
        dashLive = live;
        for (uint8_t i = 0; i < DASH_TILES; i++) {
            lv_obj_set_style_text_color(dashValues[i], lv_color_hex(live ? dashColors[i] : COLOR_DIM), 0);
            if (!live)
                setText(dashValues[i], "-");
        }
        for (uint8_t i = 0; i < DASH_RINGS; i++) {
            lv_obj_set_style_text_color(dashRingValues[i], lv_color_hex(live ? ringColors[i][1] : COLOR_DIM), 0);
            if (!live)
                setText(dashRingValues[i], "-");
            lv_obj_invalidate(dashRings[i]);
        }
    }
}

void ui_show_dashboard(bool on)
{
    if (on == dashShown)
        return;
    dashShown = on;
    // The reminder card sits over the title row of the main screen; on the
    // dashboard it covers the meeting line instead, short of the rings
    lv_obj_set_pos(remindCard, REMIND_X, on ? NEXT_Y - 10 : REMIND_Y);
    lv_obj_set_width(remindCard, on ? NEXT_W + 14 - REMIND_X : REMIND_W);
    if (on)
        updateDashboard();
    lv_scr_load(on ? dashScr : mainScr);
}

bool ui_dashboard_shown()
{
    return dashShown;
}

void ui_begin(const UiCallbacks &cb)
{
    cbs = cb;

    // Long press = "pair new device" hold time (the touch driver is LV_Helper's)
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER)
            i->driver->long_press_time = LONG_PRESS_MS;

    lv_obj_t *scr = mainScr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_pad_column(scr, 10, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = makeContainer(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, lv_pct(100));

    // Title: the name hosts see for the active slot, coloured by its state, then
    // the name given to the slot's host
    lv_obj_t *titleRow = makeContainer(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(titleRow, lv_pct(100));
    lv_obj_set_style_pad_column(titleRow, 8, 0);
    lv_obj_set_style_pad_bottom(titleRow, 6, 0);
    titleLabel = makeLabel(titleRow, &lv_font_montserrat_24, COLOR_IDLE);
    hostLabel = makeLabel(titleRow, &lv_font_montserrat_24, COLOR_AMBER);
    lv_obj_set_flex_grow(hostLabel, 1);
    lv_label_set_long_mode(hostLabel, LV_LABEL_LONG_DOT);

    // The keyboard, and the adapter's clock at the right end of its line
    lv_obj_t *usbRow = makeContainer(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(usbRow, lv_pct(100));
    lv_obj_set_style_pad_column(usbRow, 8, 0);
    usbLabel = makeLabel(usbRow, &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_flex_grow(usbLabel, 1);
    lv_label_set_long_mode(usbLabel, LV_LABEL_LONG_DOT);
    clockLabel = makeLabel(usbRow, &lv_font_montserrat_16, COLOR_DIM);
    bleLabel = makeLabel(left, &lv_font_montserrat_16, COLOR_TEXT);

    slotRow = makeContainer(left, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(slotRow, lv_pct(100));
    lv_obj_set_flex_align(slotRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slotRow, 10, 0);
    slotLabel = makeLabel(slotRow, &lv_font_montserrat_14, COLOR_NEON_HI);
    pairBtn = makePairButton(slotRow);
    agentBtn = makeAgentButton(slotRow);

    countLabel = makeLabel(left, &lv_font_montserrat_16, COLOR_GREEN);
    lv_obj_set_width(countLabel, lv_pct(100));
    lv_label_set_long_mode(countLabel, LV_LABEL_LONG_DOT); // huge totals must not run into the slots
    statsLabel = makeLabel(left, &lv_font_montserrat_12, COLOR_DIM);
    noteLabel  = makeLabel(left, &lv_font_montserrat_14, COLOR_NOTE);

    logLabel = makeLabel(left, &lv_font_montserrat_12, COLOR_DIM);
    lv_obj_set_width(logLabel, lv_pct(100));
    lv_obj_set_flex_grow(logLabel, 1);
    lv_label_set_long_mode(logLabel, LV_LABEL_LONG_CLIP);

    makeStrip(scr);
    layoutStrip();
    makeHoldBar();
    makeReminderCard(); // under the menu and the pop-ups
    makeMenu();
    makeRenamePanel();
    makeAgentCard();
    makePasskeyCard();
    makeDashboard();
}

void ui_update(const char *bootNote)
{
    BridgeStatus bs;
    bridge_get_status(bs);
    BleHidStatus ble;
    ble_hid_get_status(ble);
    char buf[96];

    snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " %s %u", BLE_DEVICE_NAME, ble.slot + 1);
    setText(titleLabel, buf);
    static uint32_t titleColor = 0; // restyle only on change (every set invalidates)
    uint32_t color = ble.ready ? COLOR_BT : ble.pairingLeftMs ? COLOR_NEON : COLOR_IDLE;
    if (color != titleColor) {
        titleColor = color;
        lv_obj_set_style_text_color(titleLabel, lv_color_hex(color), 0);
    }
    const char *host = ble.slotUsed[ble.slot] ? ble.slotName[ble.slot] : "";
    snprintf(buf, sizeof(buf), "- %s", host);
    setText(hostLabel, host[0] ? buf : "");
    uint8_t hh, mm, ss;
    if (anim_clock_now(hh, mm, ss))
        snprintf(buf, sizeof(buf), "%02u:%02u", hh, mm);
    else
        buf[0] = 0;
    setText(clockLabel, buf);

    if (ble.slot != shownSlot) // changed elsewhere (boot): no animation
        showSlot(ble.slot, false);
    bool usedChanged = false;
    for (uint8_t s = 0; s < BLE_SLOTS; s++) {
        usedChanged |= slotUsedShown[s] != ble.slotUsed[s];
        slotUsedShown[s] = ble.slotUsed[s];
    }
    if (usedChanged)
        lv_obj_invalidate(strip);

    if (bs.usbConnected)
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %s", bs.usbProduct[0] ? bs.usbProduct : "keyboard");
    else
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " waiting for the keyboard");
    setText(usbLabel, buf);

    if (!ble.started) {
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
        snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " waiting for %s",
                 ble.slotName[ble.slot][0] ? ble.slotName[ble.slot] : "the host");
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
    setVisible(passkeyCard, ble.passkeyShown);
    if (ble.passkeyShown) {
        snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)(ble.passkey / 1000),
                 (unsigned long)(ble.passkey % 1000));
        setText(passkeyLabel, buf);
    }
    setVisible(agentCard, bs.agentAsking && !ble.passkeyShown);
    if (ui_menu_open())
        updateMenu(ble);

    StatsSnapshot st;
    stats_get(st);
    char keys[24], clicks[24], dist[32];
    groupDigits(st.keys, keys, sizeof(keys));
    groupDigits(st.clicks, clicks, sizeof(clicks));
    float m = stats_meters(st);
    if (m < 1000)
        snprintf(dist, sizeof(dist), "%u m of mouse", (unsigned)m);
    else
        snprintf(dist, sizeof(dist), "%.2f km of mouse", m / 1000);
    snprintf(buf, sizeof(buf), LV_SYMBOL_KEYBOARD " %s keys | %s clicks | %s", keys, clicks, dist);
    setText(countLabel, buf);

    snprintf(buf, sizeof(buf), "fw %s %s | reports: %lu sent, %lu merged, %lu dropped", __DATE__,
             __TIME__, (unsigned long)bs.forwarded, (unsigned long)bs.merged, (unsigned long)bs.dropped);
    setText(statsLabel, buf);

    OtaStatus ota;
    ota_get_status(ota);
    char otaBuf[64];
    const char *note = bs.note ? bs.note : bootNote ? bootNote : "";
    if (ota.active && ota.total) {
        snprintf(otaBuf, sizeof(otaBuf), LV_SYMBOL_DOWNLOAD " %s %lu%%  (%lu / %lu KB)",
                 ota.animation ? "Animation upload" : "Firmware update",
                 (unsigned long)((uint64_t)ota.received * 100 / ota.total),
                 (unsigned long)(ota.received / 1024), (unsigned long)(ota.total / 1024));
        note = otaBuf;
    }
    setText(noteLabel, note);
    setVisible(noteLabel, note[0] != 0);

    static char logBuf[1024];
    if (log_screen_text(logBuf, sizeof(logBuf)))
        lv_label_set_text(logLabel, logBuf);

    if (dashShown)
        updateDashboard();
}
