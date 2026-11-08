#pragma once
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"
#include "LvImagesGen.h"
#include "PriceFormat.h"
#include "CoinPalette.h"

// -- The Coin (Binance) single-coin screen -----------------------------------------------
//
// Like the other crypto apps this was
// never ported to LVGL, so its old Display::renderApp path is a NO-OP (Display.h:107) and
// nothing repainted - the "BitCoin" launcher icon stayed on screen. This is the LVGL screen,
// lv_scr_load-ed on entry and updated in place each loop.
//
// Differs from LvCoinGecko: a 186px price panel with a SEPARATE 58px K-line interval badge
// to its right (not a full-width panel), icon+symbol vertically CENTER-aligned, and a chart
// that is a LINE by default (AppCoin _showChart=true) or CANDLES when _showChart is false.
//
// EVERY COLOUR IS RGB565 - Lv::c565, never lv_color_hex.
namespace LvCoin {

static constexpr int CHART_X = 0, CHART_Y = 104, CHART_W = 240, CHART_H = 134;
static constexpr int CANDLES_MAX = 30;

inline lv_obj_t* screen    = nullptr;
inline lv_obj_t* iconImg   = nullptr;
inline lv_obj_t* lblSymbol = nullptr;
inline lv_obj_t* pricePanel= nullptr;
inline lv_obj_t* lblPrice  = nullptr;
inline lv_obj_t* badgeBox  = nullptr;   // K-line interval badge (58x53)
inline lv_obj_t* lblBadge  = nullptr;
inline lv_obj_t* lblPct    = nullptr;
inline lv_obj_t* chartBox  = nullptr;   // chart-area group; the c_ani slide animates THIS
inline lv_obj_t* chart     = nullptr;   // line-chart mode
inline lv_chart_series_t* series = nullptr;
inline lv_obj_t* wick[CANDLES_MAX] = {};
inline lv_obj_t* body[CANDLES_MAX] = {};
inline lv_obj_t* lblSpan   = nullptr;
inline lv_obj_t* lblLoad   = nullptr;
inline lv_obj_t* lblClock  = nullptr;   // c_clock overlay badge, hidden unless c_clock

// Which theme's widget tree is currently BUILT (c_theme: 0 chart, 1 large, 2 split, 3 compact).
// Declared up here because the update*() guards below test it - it used to sit down beside the
// theme-1/2/3 widgets, i.e. after its first use. Every builder assigns it, including build().
inline int _theme = 0;

// The coin icon points straight at a flash lv_img_dsc_t (LvImagesGen.h). There used to be a
// 3 KB heap scratch buffer here, re-interleaved from split pixel/alpha arrays on every coin
// change; that split also silently mis-typed DOGE, which is TRUE_COLOR, not
// TRUE_COLOR_ALPHA.
inline const lv_img_dsc_t* _iconShown = nullptr;

inline void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}
inline void setCandlesHidden(bool hidden) {
    for (int i = 0; i < CANDLES_MAX; i++) { setHidden(wick[i], hidden); setHidden(body[i], hidden); }
}

inline void build() {
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    _theme = 0;              // every builder must state its theme; this one used to omit it
    series = nullptr;
    _iconShown = nullptr;
    for (int i = 0; i < CANDLES_MAX; i++) { wick[i] = nullptr; body[i] = nullptr; }

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0x0000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // COIN ICON - LEFT_MID (0,-94) => y-centre 26. Source set per-coin in setIcon().
    iconImg = lv_img_create(screen);
    lv_obj_align(iconImg, LV_ALIGN_LEFT_MID, 0, -94);
    lv_obj_add_flag(iconImg, LV_OBJ_FLAG_HIDDEN);

    // SYMBOL - LEFT_MID (33,-93), white. Font set per-symbol in update().
    lblSymbol = lv_label_create(screen);
    lv_label_set_text(lblSymbol, "");
    lv_obj_set_width(lblSymbol, LV_SIZE_CONTENT);
    lv_obj_set_height(lblSymbol, LV_SIZE_CONTENT);
    lv_obj_align(lblSymbol, LV_ALIGN_LEFT_MID, 33, -93);
    lv_obj_set_style_text_color(lblSymbol, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblSymbol, &FONT_SYM25, 0);

    // PRICE PANEL - 186x53, CENTER (-28,-46). Colour set by change sign.
    pricePanel = lv_obj_create(screen);
    lv_obj_set_width(pricePanel, 186);
    lv_obj_set_height(pricePanel, 53);
    lv_obj_set_align(pricePanel, LV_ALIGN_CENTER);
    lv_obj_set_x(pricePanel, -28);
    lv_obj_set_y(pricePanel, -46);
    lv_obj_clear_flag(pricePanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pricePanel, 0, 0);
    lv_obj_set_style_border_width(pricePanel, 0, 0);
    lv_obj_set_style_bg_opa(pricePanel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pricePanel, Lv::c565(0x07E0), 0);
    lv_obj_set_style_pad_all(pricePanel, 0, 0);

    lblPrice = lv_label_create(pricePanel);
    lv_label_set_text(lblPrice, "");
    lv_obj_set_width(lblPrice, LV_SIZE_CONTENT);
    lv_obj_set_height(lblPrice, LV_SIZE_CONTENT);
    lv_obj_set_align(lblPrice, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lblPrice, Lv::c565(0x0000), 0);   // BLACK on the slab
    lv_obj_set_style_text_font(lblPrice, &FONT_PRICE35, 0);

    // K-LINE INTERVAL BADGE - 58x53, CENTER (92,-46), bg 0x4A28, white text SYM25.
    badgeBox = lv_obj_create(screen);
    lv_obj_set_width(badgeBox, 58);
    lv_obj_set_height(badgeBox, 53);
    lv_obj_set_align(badgeBox, LV_ALIGN_CENTER);
    lv_obj_set_x(badgeBox, 92);
    lv_obj_set_y(badgeBox, -46);
    lv_obj_clear_flag(badgeBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(badgeBox, 0, 0);
    lv_obj_set_style_border_width(badgeBox, 0, 0);
    lv_obj_set_style_bg_opa(badgeBox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badgeBox, Lv::c565(0x4A28), 0);
    lv_obj_set_style_pad_all(badgeBox, 0, 0);

    lblBadge = lv_label_create(badgeBox);
    lv_label_set_text(lblBadge, "");
    lv_obj_set_width(lblBadge, LV_SIZE_CONTENT);
    lv_obj_set_height(lblBadge, LV_SIZE_CONTENT);
    lv_obj_set_align(lblBadge, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lblBadge, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblBadge, &FONT_SYM25, 0);

    // CHANGE % - RIGHT_MID (0,-93), coloured by sign, font PCT27.
    lblPct = lv_label_create(screen);
    lv_label_set_text(lblPct, "");
    lv_obj_set_width(lblPct, LV_SIZE_CONTENT);
    lv_obj_set_height(lblPct, LV_SIZE_CONTENT);
    lv_obj_align(lblPct, LV_ALIGN_RIGHT_MID, 0, -93);
    lv_obj_set_style_text_color(lblPct, Lv::c565(0x07E0), 0);
    lv_obj_set_style_text_font(lblPct, &FONT_PCT27, 0);

    // CHART-AREA GROUP. The c_ani effect slides the whole chart area by animating one
    // container's y, so its children move together. This container is deliberately
    // FULL-SCREEN and transparent rather than chart-sized: every chart-area object below is
    // already positioned in SCREEN coordinates (the candles use BOTTOM_LEFT with a BASE
    // offset), and a screen-sized parent keeps all of that arithmetic valid unchanged. Only
    // chart-area objects are parented here, so sliding it slides exactly the right group.
    chartBox = lv_obj_create(screen);
    lv_obj_set_size(chartBox, 240, 240);
    lv_obj_align(chartBox, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(chartBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(chartBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chartBox, 0, 0);
    lv_obj_set_style_pad_all(chartBox, 0, 0);
    lv_obj_set_style_radius(chartBox, 0, 0);

    // LINE CHART - (0,104) 240x134 within the group, dots off. Hidden until used.
    chart = lv_chart_create(chartBox);
    lv_obj_set_size(chart, CHART_W, CHART_H);
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, CHART_X, CHART_Y);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, 0, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
    series = lv_chart_add_series(chart, Lv::c565(0x07E0), LV_CHART_AXIS_PRIMARY_Y);

    // SPAN label, BOTTOM_LEFT, white BODY27.
    lblSpan = lv_label_create(screen);
    lv_label_set_text(lblSpan, "");
    lv_obj_set_style_text_color(lblSpan, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblSpan, &FONT_BODY27, 0);
    lv_obj_align(lblSpan, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // "Loading..." overlay, CENTER, white.
    lblLoad = lv_label_create(screen);
    lv_label_set_text(lblLoad, "Loading...");
    lv_obj_set_style_text_color(lblLoad, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblLoad, &FONT_BODY27, 0);
    lv_obj_align(lblLoad, LV_ALIGN_CENTER, 0, 0);

    // c_clock overlay - a "12:00" badge, CENTER, DATE22, dark semi-transparent rounded bg
    // (in AppCoin_createScreen1: bg 0x3186 opa 100, radius 5, pad 5/3). Hidden
    // unless c_clock is set; shown + updated each loop by AppCoin (mirrors).
    lblClock = lv_label_create(screen);
    lv_label_set_text(lblClock, "12:00");
    lv_obj_set_style_text_color(lblClock, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblClock, &FONT_DATE22, 0);
    lv_obj_align(lblClock, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(lblClock, Lv::c565(0x3186), 0);
    lv_obj_set_style_bg_opa(lblClock, 100, 0);
    lv_obj_set_style_radius(lblClock, 5, 0);
    lv_obj_set_style_pad_left(lblClock, 5, 0);   lv_obj_set_style_pad_right(lblClock, 5, 0);
    lv_obj_set_style_pad_top(lblClock, 3, 0);    lv_obj_set_style_pad_bottom(lblClock, 3, 0);
    lv_obj_add_flag(lblClock, LV_OBJ_FLAG_HIDDEN);

    Lv::adoptScreen(screen, &screen);
    Lv::logMem("coin");
}

// c_clock overlay: show/hide + update the "HH:MM" badge (theme 0 only - the single-coin view).
inline void setClock(bool show, int hh, int mm) {
    if (!lblClock) return;
    if (!show) { setHidden(lblClock, true); return; }
    char b[8]; snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
    lv_label_set_text(lblClock, b);
    setHidden(lblClock, false);
}

inline void setIcon(const lv_img_dsc_t* dsc, bool show) {
    if (!iconImg) return;
    if (!dsc || !show) { setHidden(iconImg, true); _iconShown = nullptr; return; }
    if (dsc != _iconShown) {
        lv_img_set_src(iconImg, dsc);
        _iconShown = dsc;
    }
    setHidden(iconImg, false);
}

// The active theme's "Loading..." label (theme 0 lblLoad, else the theme's own).
inline lv_obj_t* _activeLoad();

inline void showLoading() {
    if (!screen) return;
    setHidden(_activeLoad(), false);
}

// Overlay the white "Loading..." label ON TOP of the live header+chart (or hide it) -
// the fetch-state label (un-hidden while state==0 in).
// Must be called AFTER update(), which unconditionally hides lblLoad, so the outgoing
// price/chart stay visible under the overlay until the new data lands. The tap flow is:
// Loading appears over the old screen, data loads, Loading disappears.
inline void setLoading(bool on) {
    if (!screen) return;
    setHidden(_activeLoad(), !on);
}

// Candles: opaque red/green rects, BOTTOM_LEFT of the chart area, 30-slot layout from
// helper (param_6==0 branch). Opaque so they paint even under LV_MEM pool pressure.
inline void showCandles(const float* open, const float* high, const float* low,
                        const float* close, int n) {
    if (!screen) return;
    if (n > CANDLES_MAX) n = CANDLES_MAX;
    if (n <= 0) { setCandlesHidden(true); return; }
    setHidden(chart, true);                 // hide line mode

    const int N = 30;
    int step, bodyW;
    if      (CHART_W < 60)  { step = 2;          bodyW = 1; }
    else if (CHART_W < 210) { step = CHART_W/N;  bodyW = step - 1; }
    else                    { step = CHART_W/N;  bodyW = 6; }
    if (bodyW < 1) bodyW = 1;
    const int xoff = (CHART_W - step * N) / 2;

    float mn = low[0], mx = high[0];
    for (int i = 1; i < n; i++) { mn = fminf(mn, low[i]); mx = fmaxf(mx, high[i]); }
    float rng = mx - mn;
    if (rng < 1e-6f) rng = 1e-6f;
    // 110 px plot (per). Candles are children of `screen`; anchor their base a
    // couple of px up from the screen bottom so the plot fills the chart area (~104..238).
    auto scale = [&](float v) -> int { return (int)((v - mn) / rng * 110.0f); };
    const int BASE = 2;   // px up from screen bottom = ~y238, the chart-area floor

    for (int i = 0; i < n; i++) {
        const int x = CHART_X + xoff + i * step;
        const lv_color_t col = Lv::c565(open[i] > close[i] ? 0xF800 : 0x07E0);
        // Wick: 1px spanning low..high.
        int wickH = scale(high[i]) - scale(low[i]);
        if (wickH < 1) wickH = 1;
        if (!wick[i]) {
            wick[i] = lv_obj_create(chartBox);
            lv_obj_set_style_radius(wick[i], 0, 0);
            lv_obj_set_style_border_width(wick[i], 0, 0);
            lv_obj_set_style_bg_opa(wick[i], LV_OPA_COVER, 0);
        }
        lv_obj_set_size(wick[i], 1, wickH);
        lv_obj_align(wick[i], LV_ALIGN_BOTTOM_LEFT, x + bodyW / 2, -(BASE + scale(low[i])));
        lv_obj_set_style_bg_color(wick[i], col, 0);
        // Body: min(open,close)..max(open,close).
        const float bLo = open[i] < close[i] ? open[i] : close[i];
        const float bHi = open[i] > close[i] ? open[i] : close[i];
        int bodyH = scale(bHi) - scale(bLo);
        if (bodyH < 1) bodyH = 1;
        if (!body[i]) {
            body[i] = lv_obj_create(chartBox);
            lv_obj_set_style_radius(body[i], 0, 0);
            lv_obj_set_style_border_width(body[i], 0, 0);
            lv_obj_set_style_bg_opa(body[i], LV_OPA_COVER, 0);
        }
        lv_obj_set_size(body[i], bodyW, bodyH);
        lv_obj_align(body[i], LV_ALIGN_BOTTOM_LEFT, x, -(BASE + scale(bLo)));
        lv_obj_set_style_bg_color(body[i], col, 0);
        setHidden(wick[i], false); setHidden(body[i], false);
    }
    for (int i = n; i < CANDLES_MAX; i++) { setHidden(wick[i], true); setHidden(body[i], true); }
}

inline void showLine(const float* pts, int n) {
    if (!chart || n < 2) { setHidden(chart, true); return; }
    setCandlesHidden(true);
    lv_chart_set_point_count(chart, n);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_H);
    float mn = pts[0], mx = pts[0];
    for (int i = 1; i < n; i++) { mn = fminf(mn, pts[i]); mx = fmaxf(mx, pts[i]); }
    float rng = mx - mn;
    if (rng < 1e-6f) rng = 1e-6f;
    for (int i = 0; i < n; i++)
        lv_chart_set_next_value(chart, series, (int)((pts[i] - mn) / rng * (float)CHART_H));
    lv_chart_refresh(chart);
    setHidden(chart, false);
}

// coinId here is the Binance SYMBOL (e.g. "BTCUSDT"); icon keyed off its leading ticker.
inline void update(const char* symbol, double price, double pct, const char* interval,
                   const char* span, bool lineMode,
                   const float* open, const float* high, const float* low,
                   const float* close, const float* closes, int n) {
    if (!screen || _theme != 0) return;
    setHidden(lblLoad, true);

    // Symbol font + icon visibility by symbol LENGTH (AppCoin_renderSingle):
    //   <8 -> SYM25 + icon; 8..9 -> PCT27 + icon CLEARED; >=10 -> BODY27 + icon.
    const size_t len = symbol ? strlen(symbol) : 0;
    const bool showIcon = !(len >= 8 && len < 10);
    lv_obj_set_style_text_font(lblSymbol,
        (len < 8) ? &FONT_SYM25 : (len < 10) ? &FONT_PCT27 : &FONT_BODY27, 0);
    setIcon(showIcon ? lvCoinIconForSymbol(symbol) : nullptr, showIcon);
    if (symbol) lv_label_set_text(lblSymbol, symbol);

    const String priceS = formatCoinPrice((float)price);
    lv_obj_set_style_text_font(lblPrice, priceS.length() < 9 ? &FONT_PRICE35 : &FONT_PRICE28, 0);
    lv_label_set_text(lblPrice, priceS.c_str());

    // Colours are applied per change-sign, at the default palette:
    //   UP  (>=0): panel green, price text BLACK, pct BLACK
    //   DOWN (<0): panel red,   price text WHITE, pct WHITE
    // The price was once left BLACK with the pct coloured like the panel, which is wrong on
    // the red down-panel - it needs WHITE price and WHITE pct.
    // Palette from ccolor.json (shared with CoinGecko).
    const bool up = (pct >= 0);
    const uint16_t panelBg   = up ? CoinPalette::up_bg : CoinPalette::down_bg;
    const uint16_t textColor = up ? CoinPalette::up_fg : CoinPalette::down_fg;
    lv_obj_set_style_bg_color(pricePanel, Lv::c565(panelBg), 0);
    lv_obj_set_style_text_color(lblPrice, Lv::c565(textColor), 0);
    // The % uses the DEDICATED pct colours, NOT the price fg. It sits on the black screen, so
    // fg (black for up) made the up-coin % invisible. Up = green, down = white.
    lv_obj_set_style_text_color(lblPct, Lv::c565(up ? 0x07E0 : 0xFFFF), 0);
    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", pct);
    lv_label_set_text(lblPct, pctBuf);

    if (interval) lv_label_set_text(lblBadge, interval);
    if (span)     lv_label_set_text(lblSpan, span);

    if (lineMode) showLine(closes, n);
    else          showCandles(open, high, low, close, n);
}

// ===============================================================================
// Display themes 1/2/3 (c_theme):
//   1 "1-Coin Large"
//   2 "2-Coin Split"
//   3 "3-Coin Compact"
// (theme 0, "1-Coin Chart", is build()/update() above.)
// None of themes 1/2/3 have a chart.
// Colours are per change-sign from CoinPalette (shared ccolor.json), exactly like theme 0.
// ===============================================================================
struct Row { lv_obj_t* sym=nullptr; lv_obj_t* panel=nullptr; lv_obj_t* price=nullptr;
             lv_obj_t* badge=nullptr; lv_obj_t* pct=nullptr; };
// theme 1
inline lv_obj_t* t1Sym=nullptr; inline lv_obj_t* t1Pct=nullptr; inline lv_obj_t* t1Panel=nullptr;
inline lv_obj_t* t1Price=nullptr; inline lv_obj_t* t1Load=nullptr;
// theme 2 / 3
inline Row t2[2]; inline lv_obj_t* t2Load=nullptr;
inline Row t3[3]; inline lv_obj_t* t3Clock=nullptr; inline lv_obj_t* t3Load=nullptr;

inline lv_obj_t* _activeLoad() {
    switch (_theme) { case 1: return t1Load; case 2: return t2Load; case 3: return t3Load;
                      default: return lblLoad; }
}

// Colored panel child, align CENTER + (x,y) offset - the builders' panel idiom.
inline lv_obj_t* _mkPanel(lv_obj_t* par, int w, int h, int x, int y) {
    lv_obj_t* o = lv_obj_create(par);
    lv_obj_set_size(o, w, h);
    lv_obj_set_align(o, LV_ALIGN_CENTER); lv_obj_set_x(o, x); lv_obj_set_y(o, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}
inline lv_obj_t* _mkLabel(lv_obj_t* par, const lv_font_t* font, lv_align_t al, int x, int y,
                          uint16_t col = 0xFFFF) {
    lv_obj_t* o = lv_label_create(par);
    lv_label_set_text(o, "");
    lv_obj_set_width(o, LV_SIZE_CONTENT); lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_align(o, al, x, y);
    lv_obj_set_style_text_color(o, Lv::c565(col), 0);
    lv_obj_set_style_text_font(o, font, 0);
    return o;
}
// The ONE styling every theme applies to the coin-name label. Three calls, and none of them
// is a hardcoded grey:
//     lv_obj_set_style_text_color(lbl, coin_fg /*, default WHITE*/, 0);
//     lv_obj_set_style_bg_color  (lbl, coin_bg /*, default BLACK*/, 0);
//     lv_obj_set_style_bg_opa    (lbl, 0xFF, 0);
// THIS IS THE "grey bar" BUG. 0x4A28 - the theme-0 K-line badge grey - was hardcoded here by
// mistake, so every layout drew a light bar hugging the symbol text. At the default palette
// it should be black-on-black, i.e. nothing visible at all. Now palette-driven, so it is also
// correct for a user who sets coin_bg to something visible.
inline void _grayBar(lv_obj_t* lbl) {
    lv_obj_set_style_text_color(lbl, Lv::c565(CoinPalette::coin_fg), 0);
    lv_obj_set_style_bg_color(lbl, Lv::c565(CoinPalette::coin_bg), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(lbl, 3, 0);  lv_obj_set_style_pad_right(lbl, 3, 0);
    lv_obj_set_style_pad_top(lbl, 1, 0);   lv_obj_set_style_pad_bottom(lbl, 1, 0);
    lv_obj_set_style_radius(lbl, 3, 0);
}

// -- Theme 1: 1-Coin Large ----------------------------------------
inline void buildLarge() {
    _theme = 1; _iconShown = nullptr;
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    iconImg = lv_img_create(screen);
    lv_obj_align(iconImg, LV_ALIGN_LEFT_MID, 0, -94);
    lv_obj_add_flag(iconImg, LV_OBJ_FLAG_HIDDEN);
    t1Sym   = _mkLabel(screen, &FONT_SYM25,   LV_ALIGN_LEFT_MID, 50, -93);
    _grayBar(t1Sym);
    t1Pct   = _mkLabel(screen, &FONT_COINBIG, LV_ALIGN_CENTER,    0, -41);
    t1Panel = _mkPanel(screen, 247, 130, 2, 65);
    t1Price = _mkLabel(t1Panel, &FONT_COINHUGE, LV_ALIGN_CENTER, -1, 0, 0x0000);
    t1Load  = _mkLabel(screen, &FONT_BODY27,  LV_ALIGN_CENTER,    0, -12);
    // The text used to be MISSING here: set "Loading..." and clear HIDDEN so the label is
    // visible from the first frame.
    // Only theme 0's label had its text set here, so showLoading()/setLoading() in themes
    // 1..3 were toggling the visibility of an EMPTY label - the overlay was a no-op.
    lv_label_set_text(t1Load, "Loading...");
    Lv::adoptScreen(screen, &screen);
}
inline void updateLarge(const char* symbol, double price, double pct) {
    if (!screen || _theme != 1) return;
    setHidden(t1Load, true);
    const size_t len = symbol ? strlen(symbol) : 0;
    const bool showIcon = !(len >= 8 && len < 10);
    setIcon(showIcon ? lvCoinIconForSymbol(symbol) : nullptr, showIcon);
    // SYMBOL x AND FONT ARE LENGTH-DEPENDENT - AppCoin_renderLarge:
    //     len <  10 : set_x(50), font (SYM25)
    //     len >= 10 : set_x(20), font (BODY27)
    // We pinned x=50/SYM25 for every symbol, so a 10+ character pair (e.g. "MATICUSDT" is 9,
    // but "1000PEPEUSDT" is 12) rendered too far right in a face too large and ran off.
    if (symbol) {
        lv_obj_set_x(t1Sym, (len < 10) ? 50 : 20);
        lv_obj_set_style_text_font(t1Sym, (len < 10) ? &FONT_SYM25 : &FONT_BODY27, 0);
        lv_label_set_text(t1Sym, symbol);
    }
    // Same helper the multi-coin themes use - coin_fg text on coin_bg ( inlined in
    // the theme-1 renderer as two colour calls). Was a hardcoded grey slab here too.
    _grayBar(t1Sym);
    const bool up = (pct >= 0);
    const uint16_t bg = up ? CoinPalette::up_bg : CoinPalette::down_bg;
    const uint16_t fg = up ? CoinPalette::up_fg : CoinPalette::down_fg;
    lv_obj_set_style_bg_color(t1Panel, Lv::c565(bg), 0);
    lv_obj_set_style_text_color(t1Price, Lv::c565(fg), 0);
    // PCT colour comes from the PALETTE, not from constants. ends each branch with
    //     lv_obj_set_style_text_color(pctLabel, up ? g_wPctUpFg : g_wPctDownFg, 0)
    // i.e. pct_up_fg / pct_down_fg. We hardcoded 0x07E0/0xFFFF.
    //
    // CONSEQUENCE, stated plainly: pct_up_fg defaults to BLACK on a black screen, so at the
    // default palette a RISING coin shows NO readable percentage. That is what the palette
    // says to draw. Set pct_up_fg in the web UI, or hardcode 0x07E0/0xFFFF here, if a
    // readable-by-default percentage is wanted instead.
    lv_obj_set_style_text_color(t1Pct, Lv::c565(up ? CoinPalette::pct_up_fg
                                                  : CoinPalette::pct_down_fg), 0);
    // PRICE FONT IS LENGTH-DEPENDENT TOO: len < 8 -> (COINHUGE), else
    // (COINBIG). Measured on the FORMATTED string, not the raw number. We pinned COINHUGE, so
    // long prices overflowed the panel instead of stepping down a size.
    const String priceS = formatCoinPrice((float)price);
    lv_obj_set_style_text_font(t1Price,
        (priceS.length() < 8) ? &FONT_COINHUGE : &FONT_COINBIG, 0);
    lv_label_set_text(t1Price, priceS.c_str());
    char b[16]; snprintf(b, sizeof(b), "%+.1f%%", pct); lv_label_set_text(t1Pct, b);
}

// -- Theme 2: 2-Coin Split ----------------------------------------
// Per coin: symbol label (TOP_LEFT), a full-width COINBIG price panel, a pct badge (BODY27).
inline void buildSplit() {
    _theme = 2;
    screen = lv_obj_create(nullptr);
    // NO screen background here, deliberately. Themes 2 and 3 go straight from
    // lv_obj_create(0) + clear_flag(SCROLLABLE) into their children and never touch
    // bg_color/bg_opa, so the screen keeps the DEFAULT THEME dark grey set by
    // _initCryptoTheme(). Forcing black here is what made the coin-name label's black
    // background invisible instead of a black rectangle on grey.
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    // Coin A (top), Coin B (bottom). Geometry is CENTER-relative.
    const int panelY[2] = { -31, 87 };
    const int symY[2]   = { 12, 130 };   // TOP_LEFT
    const int symX[2]   = { 7, 9 };
    const int badgeY[2] = { -90, 28 };   // 64x55 @ x=87
    const int priceX[2] = { 1, -10 };
    for (int i = 0; i < 2; i++) {
        t2[i].panel = _mkPanel(screen, 236, 60, 0, panelY[i]);
        t2[i].price = _mkLabel(t2[i].panel, &FONT_COINBIG, LV_ALIGN_CENTER, priceX[i], -1, 0xFFFF);
        t2[i].sym   = _mkLabel(screen, &FONT_SYM25, LV_ALIGN_TOP_LEFT, symX[i], symY[i], 0xFFFF);
        _grayBar(t2[i].sym);
        t2[i].badge = _mkPanel(screen, 64, 55, 87, badgeY[i]);
        t2[i].pct   = _mkLabel(t2[i].badge, &FONT_BODY27, LV_ALIGN_CENTER, 1, -1, 0xFFFF);
    }
    t2Load = _mkLabel(screen, &FONT_BODY27, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(t2Load, "Loading...");     // see buildLarge: the text was missing
    Lv::adoptScreen(screen, &screen);
}

// -- Theme 3: 3-Coin Compact --------------------------------------
// Per coin: symbol (TOP_LEFT), a PRICE28 price panel on the right, a pct badge on the left.
inline void buildCompact() {
    _theme = 3;
    screen = lv_obj_create(nullptr);
    // NO screen background here, deliberately. Themes 2 and 3 go straight from
    // lv_obj_create(0) + clear_flag(SCROLLABLE) into their children and never touch
    // bg_color/bg_opa, so the screen keeps the DEFAULT THEME dark grey set by
    // _initCryptoTheme(). Forcing black here is what made the coin-name label's black
    // background invisible instead of a black rectangle on grey.
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    t3Clock = _mkLabel(screen, &FONT_DATE22, LV_ALIGN_CENTER, 85, -106, 0xFFFF);
    const int symY[3]   = { 3, 80, 158 };
    const int panelY[3] = { -68, 11, 87 };   // price panel 161x52 @x=37
    const int badgeY[3] = { -68, 11, 88 };    // pct badge 70x52 @x=-80
    for (int i = 0; i < 3; i++) {
        t3[i].sym   = _mkLabel(screen, &FONT_SYM25, LV_ALIGN_TOP_LEFT, 4, symY[i], 0xFFFF);
        _grayBar(t3[i].sym);
        t3[i].panel = _mkPanel(screen, 161, 52, 37, panelY[i]);
        t3[i].price = _mkLabel(t3[i].panel, &FONT_PRICE28, LV_ALIGN_CENTER, -2, -1, 0xFFFF);
        t3[i].badge = _mkPanel(screen, 70, 52, -80, badgeY[i]);
        t3[i].pct   = _mkLabel(t3[i].badge, &FONT_BODY27, LV_ALIGN_CENTER, -1, -2, 0xFFFF);
    }
    t3Load = _mkLabel(screen, &FONT_BODY27, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(t3Load, "Loading...");     // see buildLarge: the text was missing
    Lv::adoptScreen(screen, &screen);
}

// Fill one coin row (themes 2/3). symbol==nullptr hides the whole row (fewer coins than slots).
inline void _fillRow(Row& r, const char* symbol, double price, double pct, bool haveData) {
    const bool show = symbol && *symbol;
    setHidden(r.sym, !show); setHidden(r.panel, !show); setHidden(r.badge, !show);
    if (!show) return;
    lv_label_set_text(r.sym, symbol);
    const bool up = (pct >= 0);
    // PRICE panel/text use the unprefixed palette pair; the PCT BADGE uses the pct_* pair:
    //     pct label   -> pct_*_fg text on a pct_*_bg badge
    //     price label -> up_fg / down_fg
    //     lv_obj_set_style_bg_color  (pricePanel, isUp ? up_bg : down_bg)
    // We were painting the badge with the PRICE colours, so a user-configured pct palette had
    // no effect in the 2- and 3-coin layouts and the badge tracked the wrong pair.
    const uint16_t bg    = up ? CoinPalette::up_bg     : CoinPalette::down_bg;
    const uint16_t fg    = up ? CoinPalette::up_fg     : CoinPalette::down_fg;
    const uint16_t pctBg = up ? CoinPalette::pct_up_bg : CoinPalette::pct_down_bg;
    const uint16_t pctFg = up ? CoinPalette::pct_up_fg : CoinPalette::pct_down_fg;
    lv_obj_set_style_bg_color(r.panel, Lv::c565(bg), 0);
    lv_obj_set_style_bg_color(r.badge, Lv::c565(pctBg), 0);
    lv_obj_set_style_text_color(r.price, Lv::c565(fg), 0);
    lv_obj_set_style_text_color(r.pct,   Lv::c565(pctFg), 0);
    lv_label_set_text(r.price, haveData ? formatCoinPrice((float)price).c_str() : "--");
    char b[16]; snprintf(b, sizeof(b), "%+.1f%%", pct); lv_label_set_text(r.pct, b);
}
inline void updateSplit(const char* const sym[2], const double price[2], const double pct[2],
                        const bool have[2]) {
    if (!screen || _theme != 2) return;
    setHidden(t2Load, true);
    for (int i = 0; i < 2; i++) _fillRow(t2[i], sym[i], price[i], pct[i], have[i]);
}
inline void updateCompact(const char* const sym[3], const double price[3], const double pct[3],
                          const bool have[3]) {
    if (!screen || _theme != 3) return;
    setHidden(t3Load, true);
    for (int i = 0; i < 3; i++) _fillRow(t3[i], sym[i], price[i], pct[i], have[i]);
}

// EVERY widget pointer in this namespace, nulled. This MUST run before a rebuild.
//
// Why it is not optional: the previous theme's screen is deleted by Lv::adoptScreen inside the
// build* below, which takes all of its children with it - so every pointer belonging to that
// theme becomes DANGLING. Nulling only `screen` (what this did before) left the rest alive-
// looking, and the update* functions guard on `screen`, not on the individual widget. So after
// a live theme change the new screen made `screen` non-null again and the wrong theme's
// update* then wrote through freed pointers, or through pointers that were never built at all
// (nullptr) - a reboot either way. That was the /set?c_theme=1 crash.
// -- The global LVGL theme, applied before building any crypto screen ----------------
// Both crypto apps do this immediately before creating their screens:
//     lv_theme_default_init(disp, lv_palette_main(5 /*BLUE*/), lv_palette_main(0 /*RED*/),
//                           1 /*dark*/, font /*SMALL16*/);
//     lv_disp_set_theme(disp, theme);
// It matters here for one specific reason: themes 2 and 3 NEVER set their screen's background
// (verified in the bytes - AppCoin_buildScreenSplit2, AppCoin_buildScreenCompact3
// and the CoinGecko pair/ all go straight from
// lv_obj_create(0) + lv_obj_clear_flag(SCROLLABLE) into their children). So the split and
// compact layouts sit on the DEFAULT THEME's dark grey, so the coin-name label's black
// background reads as a black rectangle on grey, which is the intent.
// Themes 0 and 1 do paint their screen black explicitly, so they are unaffected.
inline void _initCryptoTheme() {
    lv_disp_t* d = lv_disp_get_default();
    lv_theme_t* th = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE),
                                           lv_palette_main(LV_PALETTE_RED),
                                           true, &FONT_SMALL16);
    lv_disp_set_theme(d, th);
}

inline void _resetWidgets() {
    // Back to theme 0 - build() (the theme-0 builder) is the ONE builder that never assigned
    // _theme, while buildLarge/buildSplit/buildCompact all do. So switching 3 -> 0 left
    // _theme == 3, and _activeLoad() then handed out t3Load, which this function had just
    // nulled: lv_label_set_text(nullptr) -> LoadProhibited. Reset it here so the default
    // branch is always consistent with the theme actually built.
    _theme = 0;
    screen = nullptr;
    iconImg = lblSymbol = pricePanel = lblPrice = nullptr;
    chartBox = nullptr;
    badgeBox = lblBadge = lblPct = chart = nullptr;
    lblSpan = lblLoad = lblClock = nullptr;
    series = nullptr;
    _iconShown = nullptr;
    for (int i = 0; i < CANDLES_MAX; i++) { wick[i] = nullptr; body[i] = nullptr; }
    t1Sym = t1Pct = t1Panel = t1Price = t1Load = nullptr;
    t2Load = t3Load = t3Clock = nullptr;
    for (int i = 0; i < 2; i++) t2[i] = Row{};
    for (int i = 0; i < 3; i++) t3[i] = Row{};
}

// -- c_ani: the chart-area slide --------------------------------------------------
// AppCoin_renderSingle onward, gated on its param 5 (the c_ani flag):
//     if (!flag) skip
//     lv_anim_del(chartContainer, exec) ; lv_anim_del(0)
//     lv_anim_init; var = chart container; exec_cb = lv_obj_set_y;
//     ready_cb = no-op; start = lv_obj_get_height(obj) = 134; end = 0x32 = 50;
//     time = 0x3e8 = 1000 ms
// i.e. the chart group slides UP 84 px into its resting position over one second. Ours moves
// the full-screen group from +84 to 0, which is the same travel and the same duration.
// One-shot: the caller fires it once per entry, not per frame.
inline void animateChartUp() {
    if (!chartBox) return;
    lv_anim_del(chartBox, nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, chartBox);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_y((lv_obj_t*)o, v); });
    lv_anim_set_values(&a, 84, 0);        // 84 px of travel
    lv_anim_set_time(&a, 1000);
    lv_anim_start(&a);
}

// Theme dispatcher - build the ONE screen the user selected. Note the alternative, which is
// to build ALL FOUR screens up front and lv_scr_load the selected one,
// so a later theme change is a pure screen swap and needs no rebuild - the renderers
// (AppCoin_renderSingle) each lv_scr_load their own theme's screen.
// We build one screen at a time because our LVGL pool cannot hold four, which means a theme
// change MUST rebuild. AppCoin::reloadConfig does that when c_theme changes.
inline void buildTheme(int theme) {
    _initCryptoTheme();
    _resetWidgets();
    switch (theme) {
        case 1:  buildLarge();   break;
        case 2:  buildSplit();   break;
        case 3:  buildCompact(); break;
        default: build();        break;   // theme 0 (single-coin chart)
    }
}

} // namespace LvCoin
