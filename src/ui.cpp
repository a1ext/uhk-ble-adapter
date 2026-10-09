#include "ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include "ble_hid.h"
#include "bridge.h"
#include "config.h"
#include "log.h"
#include "ota.h"
#include "stats.h"

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
static lv_obj_t *titleLabel;
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
    if (slot == shownSlot)
        return;
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
        lv_obj_add_event_cb(cell, slotEvent, LV_EVENT_CLICKED, (void *)(uintptr_t)s);
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

    // Title: the name hosts see for the active slot, coloured by its state
    titleLabel = makeLabel(left, &lv_font_montserrat_28, COLOR_IDLE);
    lv_obj_set_style_pad_bottom(titleLabel, 6, 0);

    usbLabel = makeLabel(left, &lv_font_montserrat_16, COLOR_TEXT);
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
    makeAgentCard();
    makePasskeyCard();
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
    setVisible(passkeyCard, ble.passkeyShown);
    if (ble.passkeyShown) {
        snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)(ble.passkey / 1000),
                 (unsigned long)(ble.passkey % 1000));
        setText(passkeyLabel, buf);
    }
    setVisible(agentCard, bs.agentAsking && !ble.passkeyShown);

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
