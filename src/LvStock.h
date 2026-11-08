#pragma once
#include <lvgl.h>
#include <math.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"

// -- The Stock screen ---------------------------------------------------------------------
//
// Built once on app entry, then updated in place.
//
// EVERY COLOUR BELOW IS RGB565 - at LV_COLOR_DEPTH 16 a style colour is one 16-bit word.
// Hence Lv::c565, never lv_color_hex.
namespace LvStock {

// -- Widgets ---------------------------------------------------------------------
inline lv_obj_t* screen    = nullptr;   // root
inline lv_obj_t* lblSymbol = nullptr;
inline lv_obj_t* priceBox  = nullptr;
inline lv_obj_t* lblPrice  = nullptr;
inline lv_obj_t* lblPct    = nullptr;
inline lv_obj_t* badgeBox  = nullptr;
inline lv_obj_t* lblBadge  = nullptr;
inline lv_obj_t* lblAbs    = nullptr;
inline lv_obj_t* panel     = nullptr;   // the chart panel
inline lv_obj_t* chart     = nullptr;
inline lv_chart_series_t* series = nullptr;
inline lv_obj_t* lblLoad   = nullptr;   // "Loading..."
inline lv_obj_t* lblSpan   = nullptr;

// Candle objects: two arrays of 24 lv_obj*, wicks and bodies. Created lazily - a slot is
// only filled while still NULL - and thereafter only re-sized, re-aligned and re-coloured.
static constexpr int CANDLES = 24;
inline lv_obj_t* wick[CANDLES] = {};
inline lv_obj_t* body[CANDLES] = {};

// Candle layout.
static constexpr int CANDLE_X0     = -13;   // first candle's x
static constexpr int CANDLE_PITCH  = 10;
static constexpr int CANDLE_YBASE  = 1;     // from LV_ALIGN_BOTTOM_LEFT
static constexpr int CANDLE_BODY_W = 7;
static constexpr int PLOT_H        = 108;   // also the lv_chart's height and y-range

// Shown in every value slot until the first successful fetch.
inline const char* const PLACEHOLDER = "--";

// -- Helpers ---------------------------------------------------------------------
inline void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Both candle arrays move together.
inline void setCandlesHidden(bool hidden) {
    for (int i = 0; i < CANDLES; i++) { setHidden(wick[i], hidden); setHidden(body[i], hidden); }
}

inline void setChartHidden(bool hidden) { setHidden(chart, hidden); }

// Market badge.
//   empty  -> label "", box hidden
//   "PRE"  -> bg 0x11A3, border 0x07EF, text 0x07EF   (green)
//   else   -> bg 0x3101, border 0xFE01, text 0xFE01   (orange)
// The align is re-applied on every call.
inline void setMarketBadge(const char* text) {
    if (!badgeBox || !lblBadge) return;
    if (!text || !*text) {
        lv_label_set_text(lblBadge, "");
        setHidden(badgeBox, true);
        return;
    }
    lv_label_set_text(lblBadge, text);
    const bool pre = (strcmp(text, "PRE") == 0);
    const uint16_t bg  = pre ? 0x11A3 : 0x3101;
    const uint16_t fg  = pre ? 0x07EF : 0xFE01;
    lv_obj_set_style_bg_color(badgeBox, Lv::c565(bg), 0);
    lv_obj_set_style_border_color(badgeBox, Lv::c565(fg), 0);
    lv_obj_set_style_text_color(lblBadge, Lv::c565(fg), 0);
    lv_obj_align(badgeBox, LV_ALIGN_TOP_RIGHT, -6, 76);
    setHidden(badgeBox, false);
}

// -- Build -----------------------------------------------------------------------
inline void build() {
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    for (int i = 0; i < CANDLES; i++) { wick[i] = nullptr; body[i] = nullptr; }
    series = nullptr;

    // ROOT: bg black, clear SCROLLABLE. Nothing else - no radius, border or padding.
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0x0000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // SYMBOL - lv_obj_align(TOP_LEFT, 5, 0), font STK30.
    lblSymbol = lv_label_create(screen);
    lv_label_set_text(lblSymbol, PLACEHOLDER);
    lv_obj_set_width(lblSymbol, LV_SIZE_CONTENT);
    lv_obj_set_height(lblSymbol, LV_SIZE_CONTENT);
    lv_obj_align(lblSymbol, LV_ALIGN_TOP_LEFT, 5, 0);
    lv_obj_set_style_text_font(lblSymbol, &FONT_STK30, 0);

    // PRICE BOX - 160x40 rounded orange slab, x -40 y -65 from CENTER.
    priceBox = lv_obj_create(screen);
    lv_obj_set_width(priceBox, 160);
    lv_obj_set_height(priceBox, 40);
    lv_obj_set_x(priceBox, -40);
    lv_obj_set_y(priceBox, -65);
    lv_obj_set_align(priceBox, LV_ALIGN_CENTER);
    lv_obj_clear_flag(priceBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(priceBox, Lv::c565(0xFE01), 0);
    lv_obj_set_style_bg_opa(priceBox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(priceBox, 10, 0);
    lv_obj_set_style_border_width(priceBox, 0, 0);

    lblPrice = lv_label_create(priceBox);
    lv_label_set_text(lblPrice, PLACEHOLDER);
    lv_obj_set_width(lblPrice, LV_SIZE_CONTENT);
    lv_obj_set_height(lblPrice, LV_SIZE_CONTENT);
    lv_obj_set_align(lblPrice, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lblPrice, Lv::c565(0x20E3), 0);
    lv_obj_set_style_text_font(lblPrice, &FONT_STK30, 0);

    // PERCENT - TOP_LEFT at (160, 45), font PCT27, recolor ON.
    lblPct = lv_label_create(screen);
    lv_label_set_text(lblPct, PLACEHOLDER);
    lv_obj_set_width(lblPct, LV_SIZE_CONTENT);
    lv_obj_set_height(lblPct, LV_SIZE_CONTENT);
    lv_obj_align(lblPct, LV_ALIGN_TOP_LEFT, 160, 45);
    lv_obj_set_style_text_color(lblPct, Lv::c565(0x07E0), 0);
    lv_obj_set_style_text_font(lblPct, &FONT_PCT27, 0);
    lv_label_set_recolor(lblPct, true);

    // MARKET BADGE - 50x24, TOP_RIGHT (-6, 76). Starts HIDDEN.
    badgeBox = lv_obj_create(screen);
    lv_obj_set_width(badgeBox, 50);
    lv_obj_set_height(badgeBox, 24);
    lv_obj_align(badgeBox, LV_ALIGN_TOP_RIGHT, -6, 76);
    lv_obj_clear_flag(badgeBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(badgeBox, 4, 0);
    lv_obj_set_style_border_width(badgeBox, 1, 0);
    lv_obj_set_style_border_color(badgeBox, Lv::c565(0xFE01), 0);
    lv_obj_set_style_bg_color(badgeBox, Lv::c565(0x3960), 0);
    lv_obj_set_style_bg_opa(badgeBox, LV_OPA_COVER, 0);
    // All four pads set individually to 0 - do NOT collapse this into pad_all.
    lv_obj_set_style_pad_left(badgeBox, 0, 0);
    lv_obj_set_style_pad_right(badgeBox, 0, 0);
    lv_obj_set_style_pad_top(badgeBox, 0, 0);
    lv_obj_set_style_pad_bottom(badgeBox, 0, 0);
    lv_obj_add_flag(badgeBox, LV_OBJ_FLAG_HIDDEN);

    lblBadge = lv_label_create(badgeBox);
    lv_label_set_text(lblBadge, "");
    lv_obj_set_width(lblBadge, LV_SIZE_CONTENT);
    lv_obj_set_height(lblBadge, LV_SIZE_CONTENT);
    lv_obj_set_align(lblBadge, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lblBadge, Lv::c565(0xFE01), 0);
    lv_obj_set_style_text_font(lblBadge, &FONT_SMALL16, 0);

    // ABSOLUTE CHANGE - aligned to the PRICE BOX, not to the screen.
    lblAbs = lv_label_create(screen);
    lv_label_set_text(lblAbs, PLACEHOLDER);
    lv_obj_set_width(lblAbs, LV_SIZE_CONTENT);
    lv_obj_set_height(lblAbs, LV_SIZE_CONTENT);
    lv_obj_align_to(lblAbs, priceBox, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    lv_obj_set_style_text_color(lblAbs, Lv::c565(0x07E0), 0);
    lv_obj_set_style_text_font(lblAbs, &FONT_STK30, 0);

    // CHART PANEL - note bg_opa 127: HALF transparent. This once caused a hang, because the
    // blend allocated a draw buffer from the 16 KB static LV_MEM pool that the 48 candle
    // objects had already pushed to ~96%. Moot since LV_MEM_CUSTOM=1 put LVGL on the heap.
    panel = lv_obj_create(screen);
    lv_obj_set_style_bg_color(panel, Lv::c565(0x0000), 0);
    lv_obj_set_style_bg_opa(panel, 127, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, Lv::c565(0x2945), 0);
    lv_obj_set_width(panel, 240);
    lv_obj_set_height(panel, 138);
    lv_obj_set_x(panel, 0);
    lv_obj_set_y(panel, 3);
    lv_obj_set_align(panel, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // LINE CHART. Starts HIDDEN; applyChartVisibility decides which mode is shown.
    chart = lv_chart_create(panel);
    lv_obj_set_size(chart, 240, PLOT_H);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_LEFT, CANDLE_X0, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, 0, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    // width AND height zero on LV_PART_INDICATOR is what suppresses the data-point dots.
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 0, LV_PART_TICKS);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
    series = lv_chart_add_series(chart, Lv::c565(0x07E0), LV_CHART_AXIS_PRIMARY_Y);

    // "Loading..." - a child of the ROOT, NOT of the panel. Starts HIDDEN.
    lblLoad = lv_label_create(screen);
    lv_label_set_text(lblLoad, "Loading...");
    lv_obj_set_style_text_color(lblLoad, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblLoad, &FONT_BODY27, 0);
    lv_obj_align(lblLoad, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_flag(lblLoad, LV_OBJ_FLAG_HIDDEN);

    // SPAN - a child of the PANEL.
    lblSpan = lv_label_create(panel);
    lv_label_set_text(lblSpan, "24x1D");
    lv_obj_set_style_text_color(lblSpan, Lv::c565(0x8C71), 0);
    lv_obj_set_style_text_font(lblSpan, &FONT_SMALL16, 0);
    lv_obj_align(lblSpan, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    Lv::adoptScreen(screen, &screen);
    Lv::logMem("stock");
}

// -- Reset to placeholders -----------------------------------------------------------
// Called from onEnter and on every symbol change.
inline void resetToPlaceholders(const char* symbol) {
    if (!screen) return;
    if (symbol && *symbol) lv_label_set_text(lblSymbol, symbol);
    lv_label_set_text(lblPrice, PLACEHOLDER);
    lv_label_set_text(lblPct,   PLACEHOLDER);
    setMarketBadge("");
    if (lblAbs) lv_label_set_text(lblAbs, PLACEHOLDER);
    // White, not green/red - the signed colours only appear once real data lands.
    lv_obj_set_style_text_color(lblPct, Lv::c565(0xFFFF), 0);
    setCandlesHidden(true);
    setChartHidden(true);
    if (lblLoad) {
        lv_label_set_text(lblLoad, "Loading...");
        setHidden(lblLoad, false);
    }
}

// -- Update the text row -------------------------------------------------------------
// priceHint is Yahoo's, clamped 0..6 - NOT a fixed 2 decimals. abs is DERIVED as
// pct * price / 100, never fetched. The pct format string for the negative case is the
// positive one minus its leading '+', so the sign comes from the value itself.
inline void updateTextRow(const char* symbol, const char* spanText,
                          float price, int priceHint, float pct, const char* marketState) {
    if (!screen) return;
    if (symbol) lv_label_set_text(lblSymbol, symbol);
    if (lblSpan && spanText) lv_label_set_text(lblSpan, spanText);

    const int dec = priceHint < 0 ? 0 : (priceHint > 6 ? 6 : priceHint);
    lv_label_set_text_fmt(lblPrice, "%.*f", dec, price);

    const float abs = pct * price / 100.0f;
    const uint16_t col = (pct > 0.0f) ? 0x07E0 : 0xF800;
    if (pct > 0.0f) lv_label_set_text_fmt(lblPct, "+%.1f%%", pct);
    else            lv_label_set_text_fmt(lblPct,  "%.1f%%", pct);
    lv_obj_set_style_text_color(lblPct, Lv::c565(col), 0);
    if (lblAbs) {
        if (pct > 0.0f) lv_label_set_text_fmt(lblAbs, "+%.2f", abs);
        else            lv_label_set_text_fmt(lblAbs,  "%.2f", abs);
        lv_obj_set_style_text_color(lblAbs, Lv::c565(col), 0);
    }
    setMarketBadge(marketState);
}

// -- Line mode ----------------------------------------------------------------------------
//
// Selected by st_chart; candles are the default, and selecting a symbol resets to them.
// The lv_chart is built hidden and applyChartVisibility swaps the two modes.
//
// The series is REPLACED on every update, not refilled. The y-range max and the scale factor
// are both PLOT_H, so the series always fills the plot exactly.
inline void showLine(const float* closes, int n, float pct) {
    if (!chart) return;
    if (n > CANDLES) n = CANDLES;
    lv_chart_set_point_count(chart, CANDLES);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, PLOT_H);
    // CREATE THE SERIES ONCE, then only recolour and refill it.
    //
    // This used to remove_series + add_series on EVERY call, and draw() calls this every loop
    // iteration (~20 Hz). Each pair frees and reallocates the series' point array, so with
    // LV_MEM_CUSTOM=1 (our LVGL is on the system heap) it was ~20 malloc/free cycles a second
    // on the same block. That is what aborted the device the moment the line chart was first
    // enabled:
    //     assert failed: heap_caps_free heap_caps.c:381
    //     (heap != NULL && "free() target pointer is outside heap areas")
    // i.e. a stale/double free. It is also pointless work - stock creates its chart objects
    // once and thereafter only re-sizes, re-aligns and re-colours them (the same note already
    // sits above showCandles). lv_chart_set_series_color does the colour part.
    // (c >> 8 & 0xf800) | (c >> 5 & 0x7e0) on 0x00ff00 / 0xff0000 -> 0x07E0 / 0xF800.
    const lv_color_t lineCol = Lv::c565(pct < 0.0f ? 0xF800 : 0x07E0);
    if (!series) {
        series = lv_chart_add_series(chart, lineCol, LV_CHART_AXIS_PRIMARY_Y);
        if (!series) return;
    } else {
        lv_chart_set_series_color(chart, series, lineCol);
    }
    // Refill from the start rather than appending - set_next_value walks a ring buffer, so
    // without this the line would scroll instead of being replaced.
    lv_chart_set_all_value(chart, series, 0);
    float mn = 0, mx = 0;
    if (n > 0) { mn = mx = closes[0]; }
    for (int i = 1; i < n; i++) { if (closes[i] < mn) mn = closes[i]; if (closes[i] > mx) mx = closes[i]; }
    const float rng = mx - mn;
    for (int i = 0; i < CANDLES; i++) {
        int y = 0;
        if (i < n && fabsf(rng) >= 1e-6f) y = (int)((closes[i] - mn) / rng * (float)PLOT_H);
        lv_chart_set_next_value(chart, series, y);
    }
    lv_chart_refresh(chart);
}

// -- Candle mode ---------------------------------------------------------------------
// Records are {open, close, high, low}. Colour is red when close <= open.
// Objects are created once and thereafter only re-sized / re-aligned / re-coloured.
struct KLine { float open, close, high, low; };

inline void showCandles(const KLine* k, int n) {
    if (!panel) return;
    if (n > CANDLES) n = CANDLES;
    if (n <= 0) return;

    // Seed from entry 0, then skip any entry whose high AND low are both zero: that is an
    // unfilled slot, not a real candle.
    float lo = k[0].low, hi = k[0].high;
    for (int i = 1; i < n; i++) {
        if (k[i].high == 0.0f && k[i].low == 0.0f) continue;
        if (k[i].low  < lo) lo = k[i].low;
        if (k[i].high > hi) hi = k[i].high;
    }
    const float rng = hi - lo;
    auto norm = [&](float v) -> int {
        if (fabsf(rng) < 1e-6f) return 0;
        return (int)((v - lo) / rng * (float)PLOT_H);
    };

    for (int i = 0; i < n; i++) {
        const int x = i * CANDLE_PITCH + CANDLE_X0;
        const lv_color_t col = Lv::c565(k[i].close <= k[i].open ? 0xF800 : 0x07E0);

        const int hWick = norm(k[i].high) - norm(k[i].low);
        const int yWick = CANDLE_YBASE - norm(k[i].low);
        if (!wick[i]) {
            wick[i] = lv_obj_create(panel);
            lv_obj_set_style_radius(wick[i], 0, 0);
            lv_obj_set_style_border_width(wick[i], 0, 0);
            lv_obj_set_style_bg_opa(wick[i], LV_OPA_COVER, 0);
        }
        lv_obj_set_size(wick[i], 1, hWick);
        lv_obj_align(wick[i], LV_ALIGN_BOTTOM_LEFT, x + CANDLE_BODY_W / 2, yWick);
        lv_obj_set_style_bg_color(wick[i], col, 0);

        const float bodyLo = k[i].open < k[i].close ? k[i].open : k[i].close;
        const int hBody = (int)(fabsf(k[i].open - k[i].close) / (fabsf(rng) < 1e-6f ? 1.0f : rng)
                                * (float)PLOT_H);
        if (!body[i]) {
            body[i] = lv_obj_create(panel);
            lv_obj_set_style_radius(body[i], 0, 0);
            lv_obj_set_style_border_width(body[i], 0, 0);
            lv_obj_set_style_bg_opa(body[i], LV_OPA_COVER, 0);
        }
        lv_obj_set_size(body[i], CANDLE_BODY_W, hBody);
        lv_obj_align(body[i], LV_ALIGN_BOTTOM_LEFT, x, CANDLE_YBASE - norm(bodyLo));
        lv_obj_set_style_bg_color(body[i], col, 0);
    }
    // Slots beyond n stay hidden.
    for (int i = n; i < CANDLES; i++) { setHidden(wick[i], true); setHidden(body[i], true); }

    // 48 lv_objs land in the pool the first time this runs, on top of a screen already at
    // ~49%. Log it once so the headroom is a measurement rather than a hope - if allocation
    // starts failing, LV_USE_ASSERT_MALLOC is off, so it degrades silently.
    static bool logged = false;
    if (!logged) { logged = true; Lv::logMem("stock+candles"); }
}

// -- Chart visibility: line vs candles -----------------------------------------------
inline void applyChartVisibility(bool lineMode) {
    setCandlesHidden(lineMode);
    setChartHidden(!lineMode);
    setHidden(lblLoad, true);
}

// -- Panel slide-up animation --------------------------------------------------------
// lv_anim on the panel: exec_cb lv_obj_set_y, 1000 ms, start = its current y (3), end = -5.
inline void animatePanelUp() {
    if (!panel) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, panel);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_y((lv_obj_t*)o, v); });
    lv_anim_set_values(&a, lv_obj_get_y(panel), -5);
    lv_anim_set_time(&a, 1000);
    lv_anim_start(&a);
}

} // namespace LvStock
