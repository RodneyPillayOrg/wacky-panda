#pragma once
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"
#include "LvImagesGen.h"
#include "PriceFormat.h"

// -- The CoinGecko single-coin screen -----------------------------------------------------
//
// The crypto apps were
// never ported to LVGL, so their old Display::renderApp path is a NO-OP under the LVGL
// renderer (Display.h:107) and nothing repainted. This screen fixes that: it is an LVGL
// screen, lv_scr_load-ed on entry, then its widgets are updated in place each loop - the
// same pattern the Stock app already uses.
//
// EVERY COLOUR IS RGB565 - use Lv::c565, never lv_color_hex (LV_COLOR_DEPTH 16).
namespace LvCoinGecko {

// -- Geometry (absolute, from the builder) ----------------------------------------
static constexpr int ICON_X = 0,  ICON_Y = 5;      // 32x32 coin icon, TOP_LEFT
static constexpr int SYM_Y  = 10;                  // symbol baseline row
static constexpr int PANEL_X = 0, PANEL_Y = 43, PANEL_W = 240, PANEL_H = 53;
static constexpr int CHART_X = 0, CHART_Y = 104, CHART_W = 240, CHART_H = 134;
static constexpr uint16_t CG_SERIES = 0x064A;      // the CoinGecko line's own green

// -- Widgets ----------------------------------------------------------------------
inline lv_obj_t* screen    = nullptr;
inline lv_obj_t* iconImg   = nullptr;
inline lv_obj_t* lblSymbol = nullptr;
inline lv_obj_t* pricePanel= nullptr;
inline lv_obj_t* lblPrice  = nullptr;
inline lv_obj_t* lblPct    = nullptr;
inline lv_obj_t* chartBox  = nullptr;   // chart-area group; the g_ani slide animates THIS
inline lv_obj_t* chart     = nullptr;
inline lv_chart_series_t* series = nullptr;
inline lv_obj_t* lblRange  = nullptr;
inline lv_obj_t* lblLoad   = nullptr;
inline lv_obj_t* lblClock  = nullptr;   // g_clock overlay badge (theme 0), hidden unless g_clock

// Which theme's widget tree is currently BUILT (g_theme: 0 chart, 1 large, 2 split, 3 compact).
// Must be declared before build() and the update*() guards that test it.
inline int _theme = 0;

// 12 static lv_img_dsc_t live in flash (LvImagesGen.h) and lv_img_set_src points straight at
// them. This used to be a 3 KB heap scratch buffer re-interleaved from split pixel/alpha
// arrays on every coin change, which additionally mis-typed DOGE: it is TRUE_COLOR, not
// TRUE_COLOR_ALPHA, so the old path read 1024 bytes past the end of its data block as an
// alpha plane.
inline const lv_img_dsc_t* _iconShown = nullptr;

inline void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// -- Build ----------------------------------------------------------------------
inline void build() {
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    _theme = 0;              // every builder must state its theme; this one used to omit it
    series = nullptr;
    _iconShown = nullptr;

    // ROOT: black, non-scrollable.
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0x0000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // COIN ICON - TOP_LEFT (0,5). Source set per-coin in setIcon().
    iconImg = lv_img_create(screen);
    lv_obj_align(iconImg, LV_ALIGN_TOP_LEFT, ICON_X, ICON_Y);
    lv_obj_add_flag(iconImg, LV_OBJ_FLAG_HIDDEN);

    // SYMBOL name - TOP_LEFT, white. x/font are set per-coin in update().
    lblSymbol = lv_label_create(screen);
    lv_label_set_text(lblSymbol, "");
    lv_obj_set_width(lblSymbol, LV_SIZE_CONTENT);
    lv_obj_set_height(lblSymbol, LV_SIZE_CONTENT);
    lv_obj_align(lblSymbol, LV_ALIGN_TOP_LEFT, 33, SYM_Y);
    lv_obj_set_style_text_color(lblSymbol, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblSymbol, &FONT_SYM25, 0);

    // PRICE PANEL - full-width 240x53 slab at (0,43), colour set by change sign.
    // fillRect in the sprite path => square corners, no border.
    pricePanel = lv_obj_create(screen);
    lv_obj_set_width(pricePanel, PANEL_W);
    lv_obj_set_height(pricePanel, PANEL_H);
    lv_obj_align(pricePanel, LV_ALIGN_TOP_LEFT, PANEL_X, PANEL_Y);
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

    // CHANGE % - RIGHT-aligned at (240,27), coloured by sign, font PCT27.
    lblPct = lv_label_create(screen);
    lv_label_set_text(lblPct, "");
    lv_obj_set_width(lblPct, LV_SIZE_CONTENT);
    lv_obj_set_height(lblPct, LV_SIZE_CONTENT);
    lv_obj_align(lblPct, LV_ALIGN_TOP_RIGHT, 0, 14);   // right edge; y centred ~27 for lh27
    lv_obj_set_style_text_color(lblPct, Lv::c565(0x07E0), 0);
    lv_obj_set_style_text_font(lblPct, &FONT_PCT27, 0);

    // CHART-AREA GROUP for the g_ani slide - see LvCoin::animateChartUp for the RE. This app
    // is line-only, so the group holds just the chart, but the container is still needed
    // because the animation must move the chart WITHOUT the per-frame update fighting it.
    chartBox = lv_obj_create(screen);
    lv_obj_set_size(chartBox, 240, 240);
    lv_obj_align(chartBox, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(chartBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(chartBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chartBox, 0, 0);
    lv_obj_set_style_pad_all(chartBox, 0, 0);
    lv_obj_set_style_radius(chartBox, 0, 0);

    // LINE CHART - (0,104) 240x134 within the group. Dots off.
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
    series = lv_chart_add_series(chart, Lv::c565(CG_SERIES), LV_CHART_AXIS_PRIMARY_Y);

    // RANGE label - BOTTOM_LEFT, white, font BODY27 (default "30d").
    lblRange = lv_label_create(screen);
    lv_label_set_text(lblRange, "30d");
    lv_obj_set_style_text_color(lblRange, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblRange, &FONT_BODY27, 0);
    lv_obj_align(lblRange, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // "Loading..." - CENTER, white, shown until the first price lands.
    lblLoad = lv_label_create(screen);
    lv_label_set_text(lblLoad, "Loading...");
    lv_obj_set_style_text_color(lblLoad, Lv::c565(0xFFFF), 0);
    lv_obj_set_style_text_font(lblLoad, &FONT_BODY27, 0);
    lv_obj_align(lblLoad, LV_ALIGN_CENTER, 0, 0);

    // g_clock overlay - "HH:MM" badge over the chart when g_clock is set. The Coin app's
    // c_clock is the same thing, and every theme carries the label.
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
    Lv::logMem("coingecko");
}

// g_clock overlay: show/hide + update "HH:MM" (theme 0 only). Null-safe in other themes.
inline void setClock(bool show, int hh, int mm) {
    if (!lblClock) return;
    if (!show) { setHidden(lblClock, true); return; }
    char b[8]; snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
    lv_label_set_text(lblClock, b);
    setHidden(lblClock, false);
}

// Point the image at a coin's flash descriptor and show it (or hide when none/!show).
inline void setIcon(const lv_img_dsc_t* dsc, bool show) {
    if (!iconImg) return;
    if (!dsc || !show) { setHidden(iconImg, true); _iconShown = nullptr; return; }
    if (dsc != _iconShown) {
        lv_img_set_src(iconImg, dsc);
        _iconShown = dsc;
    }
    setHidden(iconImg, false);
}

inline lv_obj_t* _activeLoad();   // active theme's Loading/status label (defined below)

// Show the placeholder, hide the data widgets. Called before the first price.
inline void showLoading() {
    if (!screen) return;
    setHidden(_activeLoad(), false);
    if (_theme == 0) setHidden(chart, true);   // chart exists only in theme 0
}

// Overlay the white "Loading..." label ON TOP of the live header+chart (or hide it) - the
// per-coin fetch state (CoinGecko_showLoadingText, white 0xFFFF, default text
// "Loading..."). Does NOT touch the chart, so the outgoing graph stays visible under it until
// the new one lands. Resets text+colour in case a coral status message left the label dirty.
inline void setChartLoading(bool on) {
    if (!screen) return;
    lv_obj_t* L = _activeLoad();
    if (on) {
        lv_label_set_text(L, "Loading...");
        lv_obj_set_style_text_color(L, Lv::c565(0xFFFF), 0);
    }
    setHidden(L, !on);
}

// Overlay a CORAL status/error message on the SAME label the "Loading..." text uses: all four
// view labels take colour 0xFB4D, and an empty message becomes "No data". Messages routed
// here:
// "Server rate limit" (429), "HTTP %d" (other non-200), "No data", "No symbol", "JSON error".
// A null/empty message clears the overlay.
inline void setStatus(const char* msg) {
    // Guard the LABEL, not just the screen: the two are independent, and `screen` being
    // non-null says nothing about the active theme's status label existing. setHidden() is
    // null-safe but lv_label_set_text() is not - that asymmetry is what turned one stale
    // pointer into a reboot.
    lv_obj_t* L = screen ? _activeLoad() : nullptr;
    if (!L) return;
    if (!msg || !*msg) { setHidden(L, true); return; }
    lv_label_set_text(L, msg);
    lv_obj_set_style_text_color(L, Lv::c565(0xFB4D), 0);   // coral, per showStatusText
    setHidden(L, false);
}

// -- Update - transcribes AppCoinGecko::_render's data path -----------------------
// name is the coin id with its first letter capitalised. price/chg are the batch-price
// values. chart[] holds chartLen close prices. range is the span label ("30d").
inline void update(const char* coinId, const char* name, double price, double chg,
                   const float* chart_pts, int chartLen, const char* range) {
    if (!screen || _theme != 0) return;
    setHidden(lblLoad, true);

    // Icon + symbol x/font, all keyed off the id LENGTH:
    //   len <= 8 -> x 33, SYM25, icon shown; 9..11 -> x 2, SYM25, icon cleared;
    //   len >= 12 -> x 0, BODY27, icon shown.
    const size_t idLen = coinId ? strlen(coinId) : 0;
    const int    symX  = (idLen < 12) ? ((idLen > 8) ? 2 : 33) : 0;
    const bool   showIcon = !(idLen > 8 && idLen < 12);
    setIcon(lvCoinIconForId(coinId), showIcon);
    lv_obj_set_style_text_font(lblSymbol, (idLen < 12) ? &FONT_SYM25 : &FONT_BODY27, 0);
    lv_obj_align(lblSymbol, LV_ALIGN_TOP_LEFT, symX, SYM_Y);
    if (name) lv_label_set_text(lblSymbol, name);

    // Price: length-driven font choice (strlen < 9 -> 35, else 28).
    const String priceS = formatCoinPrice((float)price);
    lv_obj_set_style_text_font(lblPrice, priceS.length() < 9 ? &FONT_PRICE35 : &FONT_PRICE28, 0);
    lv_label_set_text(lblPrice, priceS.c_str());

    // Colours per helper (CoinGecko renderSingle). Panel + % are green(up)/red(down);
    // the PRICE text uses the theme block helper (up 0x0000 BLACK / down 0xFFFF WHITE) -
    // NOT black always. (Unlike the Coin app, CoinGecko's % is green/red, not white/black.)
    // The price was once left BLACK, which on the red down-panel is near-invisible; it
    // should be white.
    const bool up = (chg >= 0);
    const uint16_t panelBg = up ? CoinPalette::up_bg : CoinPalette::down_bg;
    lv_obj_set_style_bg_color(pricePanel, Lv::c565(panelBg), 0);
    lv_obj_set_style_text_color(lblPrice, Lv::c565(up ? CoinPalette::up_fg : CoinPalette::down_fg), 0);
    lv_obj_set_style_text_color(lblPct,   Lv::c565(up ? 0x07E0 : 0xF800), 0);   // % FIXED green/red
    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", chg);
    lv_label_set_text(lblPct, pctBuf);

    if (range) lv_label_set_text(lblRange, range);

    // Line chart. Normalise to 0..CHART_H like LvStock::showLine; series colour fixed.
    if (chart_pts && chartLen > 1) {
        int n = chartLen;
        lv_chart_set_point_count(chart, n);
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_H);
        float mn = chart_pts[0], mx = chart_pts[0];
        for (int i = 1; i < n; i++) { mn = fminf(mn, chart_pts[i]); mx = fmaxf(mx, chart_pts[i]); }
        float rng = mx - mn;
        if (rng < 1e-6f) rng = 1e-6f;
        for (int i = 0; i < n; i++)
            lv_chart_set_next_value(chart, series,
                (int)((chart_pts[i] - mn) / rng * (float)CHART_H));
        lv_chart_refresh(chart);
        setHidden(chart, false);
    } else {
        setHidden(chart, true);
    }
}

// ===============================================================================
// Display themes 1/2/3 (g_theme). Geometry is IDENTICAL to the Coin app's, coord for coord.
// CoinGecko specifics: coin NAME (capitalised) not a symbol; theme-1 name is CENTERED with
// FONT_CGPRICE29 and NO gray bar; the % is CoinGecko's FIXED green/red where it sits on the
// black screen (theme 1). Panel/price colours are the shared CoinPalette, per change-sign.
// ===============================================================================
struct Row { lv_obj_t* sym=nullptr; lv_obj_t* panel=nullptr; lv_obj_t* price=nullptr;
             lv_obj_t* badge=nullptr; lv_obj_t* pct=nullptr; };
inline lv_obj_t* t1Name=nullptr; inline lv_obj_t* t1Pct=nullptr; inline lv_obj_t* t1Panel=nullptr;
inline lv_obj_t* t1Price=nullptr; inline lv_obj_t* t1Load=nullptr;
inline Row t2[2]; inline lv_obj_t* t2Load=nullptr;
inline Row t3[3]; inline lv_obj_t* t3Clock=nullptr; inline lv_obj_t* t3Load=nullptr;

inline lv_obj_t* _activeLoad() {
    switch (_theme) { case 1: return t1Load; case 2: return t2Load; case 3: return t3Load;
                      default: return lblLoad; }
}
inline lv_obj_t* _mkPanel(lv_obj_t* par, int w, int h, int x, int y) {
    lv_obj_t* o = lv_obj_create(par);
    lv_obj_set_size(o, w, h);
    lv_obj_set_align(o, LV_ALIGN_CENTER); lv_obj_set_x(o, x); lv_obj_set_y(o, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, 0, 0); lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0); lv_obj_set_style_pad_all(o, 0, 0);
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
// The coin-name label's styling: text = coin_fg (default WHITE), bg = coin_bg (default
// BLACK), opa 0xFF. A hardcoded 0x4A28 here - the K-line badge grey, borrowed by mistake -
// is why the multi-coin layouts once drew a light bar behind the name instead of
// black-on-black. See the matching note in LvCoin::_grayBar.
inline void _grayBar(lv_obj_t* lbl) {   // coin-name bar (multi-coin themes)
    lv_obj_set_style_text_color(lbl, Lv::c565(CoinPalette::coin_fg), 0);
    lv_obj_set_style_bg_color(lbl, Lv::c565(CoinPalette::coin_bg), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(lbl, 3, 0); lv_obj_set_style_pad_right(lbl, 3, 0);
    lv_obj_set_style_pad_top(lbl, 1, 0);  lv_obj_set_style_pad_bottom(lbl, 1, 0);
    lv_obj_set_style_radius(lbl, 3, 0);
}

inline void buildLarge() {   // g_theme 1: 1-Coin Large
    _theme = 1; _iconShown = nullptr;
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    iconImg = lv_img_create(screen);
    lv_obj_align(iconImg, LV_ALIGN_LEFT_MID, 0, -94);
    lv_obj_add_flag(iconImg, LV_OBJ_FLAG_HIDDEN);
    // Name font is set by length in updateLarge (SYM25/BODY27 - letters). The builder's
    // CGPRICE29 is a digits-only price face (placeholder the render overrides); using it for
    // the name renders tofu. Start with SYM25 so a letters face is always active.
    t1Name  = _mkLabel(screen, &FONT_SYM25, LV_ALIGN_CENTER, 0, -93);  // centered name
    t1Pct   = _mkLabel(screen, &FONT_COINBIG,   LV_ALIGN_CENTER, 0, -41);
    t1Panel = _mkPanel(screen, 247, 130, 2, 65);
    t1Price = _mkLabel(t1Panel, &FONT_COINHUGE, LV_ALIGN_CENTER, -1, 0, 0x0000);
    t1Load  = _mkLabel(screen, &FONT_BODY27,    LV_ALIGN_CENTER, 0, -12);
    lv_label_set_text(t1Load, "Loading...");   // _mkLabel leaves it EMPTY; see LvCoin::buildLarge
    Lv::adoptScreen(screen, &screen);
}
inline void updateLarge(const char* coinId, const char* name, double price, double chg) {
    if (!screen || _theme != 1) return;
    setHidden(t1Load, true);
    setIcon(lvCoinIconForId(coinId), true);
    if (name) {
        // letters font by length, mirroring theme-0 update() (SYM25 short, BODY27 long).
        lv_obj_set_style_text_font(t1Name, (strlen(name) < 12) ? &FONT_SYM25 : &FONT_BODY27, 0);
        lv_label_set_text(t1Name, name);
    }
    const bool up = (chg >= 0);
    lv_obj_set_style_bg_color(t1Panel, Lv::c565(up ? CoinPalette::up_bg : CoinPalette::down_bg), 0);
    lv_obj_set_style_text_color(t1Price, Lv::c565(up ? CoinPalette::up_fg : CoinPalette::down_fg), 0);
    lv_obj_set_style_text_color(t1Pct, Lv::c565(up ? 0x07E0 : 0xF800), 0);   // FIXED green/red on black
    lv_label_set_text(t1Price, formatCoinPrice((float)price).c_str());
    char b[16]; snprintf(b, sizeof(b), "%+.2f%%", chg); lv_label_set_text(t1Pct, b);
}

inline void buildSplit() {   // g_theme 2: 2-Coin Split
    _theme = 2;
    screen = lv_obj_create(nullptr);
    // NO screen background here, deliberately. g_theme 2 and 3 go straight from
    // lv_obj_create(0) + clear_flag(SCROLLABLE) into their children and never touch
    // bg_color/bg_opa, so the screen keeps the DEFAULT THEME dark grey from
    // _initCryptoTheme(). Forcing black is
    // what made the coin-name label's black background invisible instead of a black
    // rectangle on grey.
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    const int panelY[2] = { -31, 87 }, symY[2] = { 12, 130 }, symX[2] = { 7, 9 };
    const int badgeY[2] = { -90, 28 }, priceX[2] = { 1, -10 };
    for (int i = 0; i < 2; i++) {
        t2[i].panel = _mkPanel(screen, 236, 60, 0, panelY[i]);
        t2[i].price = _mkLabel(t2[i].panel, &FONT_COINBIG, LV_ALIGN_CENTER, priceX[i], -1, 0xFFFF);
        t2[i].sym   = _mkLabel(screen, &FONT_SYM25, LV_ALIGN_TOP_LEFT, symX[i], symY[i], 0xFFFF);
        _grayBar(t2[i].sym);
        t2[i].badge = _mkPanel(screen, 64, 55, 87, badgeY[i]);
        t2[i].pct   = _mkLabel(t2[i].badge, &FONT_BODY27, LV_ALIGN_CENTER, 1, -1, 0xFFFF);
    }
    t2Load = _mkLabel(screen, &FONT_BODY27, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(t2Load, "Loading...");   // was empty, so the overlay was a no-op
    Lv::adoptScreen(screen, &screen);
}
inline void buildCompact() {   // g_theme 3: 3-Coin Compact
    _theme = 3;
    screen = lv_obj_create(nullptr);
    // NO screen background here, deliberately. g_theme 2 and 3 go straight from
    // lv_obj_create(0) + clear_flag(SCROLLABLE) into their children and never touch
    // bg_color/bg_opa, so the screen keeps the DEFAULT THEME dark grey from
    // _initCryptoTheme(). Forcing black is
    // what made the coin-name label's black background invisible instead of a black
    // rectangle on grey.
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    t3Clock = _mkLabel(screen, &FONT_DATE22, LV_ALIGN_CENTER, 85, -106, 0xFFFF);
    const int symY[3] = { 3, 80, 158 }, panelY[3] = { -68, 11, 87 }, badgeY[3] = { -68, 11, 88 };
    for (int i = 0; i < 3; i++) {
        t3[i].sym   = _mkLabel(screen, &FONT_SYM25, LV_ALIGN_TOP_LEFT, 4, symY[i], 0xFFFF);
        _grayBar(t3[i].sym);
        t3[i].panel = _mkPanel(screen, 161, 52, 37, panelY[i]);
        t3[i].price = _mkLabel(t3[i].panel, &FONT_PRICE28, LV_ALIGN_CENTER, -2, -1, 0xFFFF);
        t3[i].badge = _mkPanel(screen, 70, 52, -80, badgeY[i]);
        t3[i].pct   = _mkLabel(t3[i].badge, &FONT_BODY27, LV_ALIGN_CENTER, -1, -2, 0xFFFF);
    }
    t3Load = _mkLabel(screen, &FONT_BODY27, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(t3Load, "Loading...");   // was empty, so the overlay was a no-op
    Lv::adoptScreen(screen, &screen);
}
inline void _fillRow(Row& r, const char* name, double price, double chg, bool have) {
    const bool show = name && *name;
    setHidden(r.sym, !show); setHidden(r.panel, !show); setHidden(r.badge, !show);
    if (!show) return;
    lv_label_set_text(r.sym, name);
    const bool up = (chg >= 0);
    // Price panel/text = the unprefixed pair; the PCT BADGE = the pct_* pair, via
    // CoinGecko_setPctColours (pct_*_fg text on a pct_*_bg background object).
    // We were painting the badge with the price colours, so the pct palette had no effect.
    const uint16_t bg    = up ? CoinPalette::up_bg     : CoinPalette::down_bg;
    const uint16_t fg    = up ? CoinPalette::up_fg     : CoinPalette::down_fg;
    const uint16_t pctBg = up ? CoinPalette::pct_up_bg : CoinPalette::pct_down_bg;
    const uint16_t pctFg = up ? CoinPalette::pct_up_fg : CoinPalette::pct_down_fg;
    lv_obj_set_style_bg_color(r.panel, Lv::c565(bg), 0);
    lv_obj_set_style_bg_color(r.badge, Lv::c565(pctBg), 0);
    lv_obj_set_style_text_color(r.price, Lv::c565(fg), 0);
    lv_obj_set_style_text_color(r.pct,   Lv::c565(pctFg), 0);
    lv_label_set_text(r.price, have ? formatCoinPrice((float)price).c_str() : "--");
    char b[16]; snprintf(b, sizeof(b), "%+.1f%%", chg); lv_label_set_text(r.pct, b);
}
inline void updateSplit(const char* const nm[2], const double pr[2], const double pc[2],
                        const bool have[2]) {
    if (!screen || _theme != 2) return; setHidden(t2Load, true);
    for (int i = 0; i < 2; i++) _fillRow(t2[i], nm[i], pr[i], pc[i], have[i]);
}
inline void updateCompact(const char* const nm[3], const double pr[3], const double pc[3],
                          const bool have[3]) {
    if (!screen || _theme != 3) return; setHidden(t3Load, true);
    for (int i = 0; i < 3; i++) _fillRow(t3[i], nm[i], pr[i], pc[i], have[i]);
}
// Every widget pointer in this namespace, nulled - MUST run before a rebuild. The outgoing
// screen is deleted by Lv::adoptScreen inside the build* below, taking all of its children
// with it, so every pointer belonging to the previous theme becomes DANGLING. The update*
// functions guard on `screen` only, so a stale or never-built pointer reaches
// lv_label_set_text and reboots the device. Identical to LvCoin::_resetWidgets - see the note
// on LvCoin::buildTheme for why a theme change rebuilds at all.
//
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
    // See LvCoin::_resetWidgets: build() is the only builder that never set _theme, so a
    // 3 -> 0 switch left _theme == 3 and _activeLoad() returned the just-nulled t3Load.
    // That is the LoadProhibited crash in setStatus() -> lv_label_set_text(nullptr),
    // backtrace (LvCoinGecko.h:226).
    _theme = 0;
    screen = nullptr;
    iconImg = lblSymbol = pricePanel = lblPrice = lblPct = chart = nullptr;
    chartBox = nullptr;
    lblRange = lblLoad = lblClock = nullptr;
    series = nullptr;
    _iconShown = nullptr;
    t1Name = t1Pct = t1Panel = t1Price = t1Load = nullptr;
    t2Load = t3Load = t3Clock = nullptr;
    for (int i = 0; i < 2; i++) t2[i] = Row{};
    for (int i = 0; i < 3; i++) t3[i] = Row{};
}

// g_ani chart slide - identical mechanism to LvCoin::animateChartUp: lv_obj_set_y, 84 px,
// 1000 ms.
inline void animateChartUp() {
    if (!chartBox) return;
    lv_anim_del(chartBox, nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, chartBox);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_y((lv_obj_t*)o, v); });
    lv_anim_set_values(&a, 84, 0);
    lv_anim_set_time(&a, 1000);
    lv_anim_start(&a);
}

inline void buildTheme(int theme) {
    _initCryptoTheme();
    _resetWidgets();
    switch (theme) { case 1: buildLarge(); break; case 2: buildSplit(); break;
                     case 3: buildCompact(); break; default: build(); break; }
}

} // namespace LvCoinGecko
