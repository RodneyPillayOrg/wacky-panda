#pragma once
#include <lvgl.h>
#include <string.h>
#include <stdlib.h>
#include "LvFontsGen.h"
#include "LvImagesGen.h"
#include "LvglDisplay.h"
#include "LvClock.h"   // the shared timecolor buffers

// -- The Weather screens -----------------------------------------------------------------
//
// Two screens: screen 0 is the main weather view, screen 1 the big digital clock.
//
// The tick cadence lives in the app (AppWeather.h): the seconds label updates when the
// second changes, clock digits and date only when the minute changes, and the weather values
// only on fetch.
namespace LvWeather {

// -- Format strings -----------------------------------------------------------------
// The placeholder detail line, args (0, 0, "0m/s", 1000):
inline constexpr const char* FMT_DETAIL_PH =
    "min temp %d\xC2\xB0" "C,max temp %d\xC2\xB0" "C,wind speed %s,atm %dhPa";
// The live detail line, args (min, max, feels, wind, atm), all strings:
inline constexpr const char* FMT_DETAIL =
    "Lowest #99ffff %s#, Highest #feba00 %s#, Feels like #feba00 %s#, "
    "Wind speed #99ff1f %s#, ATM #feba00 %s#";
inline constexpr const char* FMT_DIGIT_PLAIN  = "%01d";
inline constexpr const char* FMT_DIGIT_ORANGE = "#ffa500 %01d#";
inline constexpr const char* FMT_SECONDS_PH   = "%02d";
inline constexpr const char* FMT_DATE_PH      = "%2d/%2d/%d %s";
inline constexpr const char* FMT_TEMP_PH      = "%2d\xC2\xB0" "C";
// The pre-fetch placeholder. This was once the literal "50%%", set with the non-fmt setter
// so BOTH '%' characters landed on the panel - which reads as a typo on a screen the user
// sees before the first fetch.
inline constexpr const char* TXT_HUMI_PH      = "70%";
inline constexpr const char* FMT_DIGIT   = "%s %01d#";
inline constexpr const char* FMT_SECONDS = "%s %02d#";            //+7
inline constexpr const char* FMT_HUMI    = "%d%%";
// Date formats 1..5 (mode 4 and 5 share the last literal, swapped args). Note the
// THREE spaces in the 4/5 form - read from.
inline constexpr const char* FMT_DATE_1 = "%2d/%d/%d #87cefa %s#";
inline constexpr const char* FMT_DATE_2 = "%d/%d/%2d #87cefa %s#";
inline constexpr const char* FMT_DATE_3 = "%d/%2d/%d #87cefa %s#";
inline constexpr const char* FMT_DATE_45 = "%2d / %2d   #87cefa %s#";
// Weekday table, FIVE-byte stride, "Thu" - deliberately different from the
// Clock app's 10-byte-stride table spelling "Thur".
inline const char* const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

// Proven palette entries (lv_palette_main table at, RGB565):
inline constexpr uint16_t PAL_0  = 0xF206;   // Material red   - temp bar indicator
inline constexpr uint16_t PAL_5  = 0x24BE;   // Material blue  - humidity bar / cond badge
inline constexpr uint16_t PAL_14 = 0xFCC0;   // country badge background

// OWM icon-number lookup, the two 9-entry tables scanned in lock-step
// (codes, dscs). Default = first entry (code 1).
inline const lv_img_dsc_t* iconFor(int code) {
    for (int i = 0; i < LV_WX_ICON_COUNT; i++)
        if (LV_WX_ICON_CODES[i] == code) return LV_WX_ICONS[i];
    return LV_WX_ICONS[0];
}

// -- Screen 0: the main weather screen --------------------------------------------
inline lv_obj_t* screen       = nullptr;
inline lv_obj_t* iconImg      = nullptr;
inline lv_obj_t* condBadge    = nullptr;   // 72x25 @ (168,80), palette(5)
inline lv_obj_t* condLbl      = nullptr;
inline lv_obj_t* cityLbl      = nullptr;
inline lv_obj_t* countryBadge = nullptr;   // 50x25 @ (120,15), palette(14)
inline lv_obj_t* countryLbl   = nullptr;
inline lv_obj_t* detailLbl    = nullptr;   // the marquee
inline lv_obj_t* hourT        = nullptr;
inline lv_obj_t* hourU        = nullptr;
inline lv_obj_t* sepLbl       = nullptr;
inline lv_obj_t* minT         = nullptr;
inline lv_obj_t* minU         = nullptr;
inline lv_obj_t* secLbl       = nullptr;
inline lv_obj_t* dateLbl      = nullptr;
inline lv_obj_t* gaugeTempImg = nullptr;
inline lv_obj_t* tempBar      = nullptr;
inline lv_obj_t* tempLbl      = nullptr;
inline lv_obj_t* gaugeHumiImg = nullptr;
inline lv_obj_t* humiBar      = nullptr;
inline lv_obj_t* humiLbl      = nullptr;

// Both gauge icons are now baked at their FINAL on-screen size in assets.toml (WX_GAUGE_TEMP
// 17x32, WX_GAUGE_HUMI 22x22) -- no lv_img_set_zoom at draw time. This used to be runtime
// zoom (humidity at the Ghidra-verified 180/256=70%, temp bumped to a one-off 80% because it
// read too small next to the humidity drop). Runtime zoom pivots on the CENTER of the
// UNSCALED source box, which stayed fixed while temp's box grew -- that is what put the
// rendered temp icon visibly right of the humidity icon's midpoint. Baking the target size
// in removes the pivot mismatch entirely: both icons' centres now fall exactly where their
// alignment offset below says they do, so a plain LEFT_MID offset is enough to centre-align
// them as a column. (The 32px temp height is still the same ceiling as before: LEFT_MID at
// y+70 puts its centre at y=190, and dateLbl's bottom edge sits at ~174, leaving 16px of
// headroom either side.)

// -- Screen 1: the big digital clock ----------------------------------------------
inline lv_obj_t* scr1         = nullptr;
inline lv_obj_t* s1Icon       = nullptr;
inline lv_obj_t* s1Temp       = nullptr;
inline lv_obj_t* s1HourT      = nullptr;
inline lv_obj_t* s1Cond       = nullptr;
inline lv_obj_t* s1MinT       = nullptr;
inline lv_obj_t* s1Colon      = nullptr;
inline lv_obj_t* s1HourU      = nullptr;
inline lv_obj_t* s1MinU       = nullptr;

// Six styles, initialised once.
inline lv_style_t stScreen, stBody, stLead, stClock, stBadge, stBar;
inline bool stylesReady = false;

inline void initStyles() {
    if (stylesReady) return;
    stylesReady = true;
    // Global theme: lv_theme_default_init(disp, palette BLUE, palette RED, dark, SMALL16)
    // then lv_disp_set_theme. REQUIRED for the badges - they rely on this theme's default
    // lv_obj padding to centre their labels. (The badge "line" was a stray scrollbar, now
    // cleared.) The same theme is used across every screen, so setting it globally is right.
    {
        lv_disp_t* d = lv_disp_get_default();
        lv_theme_t* th = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE),
                                               lv_palette_main(LV_PALETTE_RED),
                                               true, &FONT_SMALL16);
        lv_disp_set_theme(d, th);
    }
    lv_style_init(&stScreen);
    lv_style_set_bg_color(&stScreen, lv_color_hex(0x000000));  //   prop 0x20
    lv_style_init(&stBody);
    lv_style_set_text_opa(&stBody, LV_OPA_COVER);
    lv_style_set_text_color(&stBody, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&stBody, &FONT_BODY27);
    lv_style_init(&stLead);
    lv_style_set_text_opa(&stLead, LV_OPA_COVER);
    lv_style_set_text_color(&stLead, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&stLead, &FONT_LEAD31);
    lv_style_init(&stClock);
    lv_style_set_text_opa(&stClock, LV_OPA_COVER);
    lv_style_set_text_color(&stClock, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&stClock, &FONT_CLOCK62);
    lv_style_init(&stBadge);
    lv_style_set_border_width(&stBadge, 0);                    //   prop 0x32
    lv_style_init(&stBar);
    lv_style_set_bg_color(&stBar, lv_color_hex(0x000000));     //   prop 0x20
    lv_style_set_border_width(&stBar, 2);                      //   prop 0x32
    lv_style_set_border_color(&stBar, lv_color_hex(0xFFFFFF)); //   prop 0x30
    lv_style_set_pad_top(&stBar, 1);                           //   props 0x10..0x13
    lv_style_set_pad_bottom(&stBar, 1);
    lv_style_set_pad_left(&stBar, 1);
    lv_style_set_pad_right(&stBar, 1);
}

// The marquee exec callback (drives the label's X).
inline void marqueeExec(void* obj, int32_t v) { lv_obj_set_x((lv_obj_t*)obj, v); }

// Builds the placeholder state; data arrives via updateMain()/setClockAndDate().
//
// Only ONE of the two screens is alive at a time - Lv::adoptScreen destroys the other on
// toggle and the toggle path rebuilds fresh. Keeping both would be nicer, but not at
// LV_MEM_SIZE 16 KB.
inline void buildMain() {
    initStyles();
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    iconImg = condBadge = condLbl = cityLbl = countryBadge = countryLbl = nullptr;
    detailLbl = hourT = hourU = sepLbl = minT = minU = secLbl = dateLbl = nullptr;
    gaugeTempImg = tempBar = tempLbl = gaugeHumiImg = humiBar = humiLbl = nullptr;

    screen = lv_obj_create(nullptr);
    // Original order: set_style_bg_color(screen, 0, 0) then add_style(stScreen).
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_add_style(screen, &stScreen, 0);
    // The detail MARQUEE animates detailLbl's x off-screen (to -20-labelWidth), extending the
    // screen's scrollable area left. With scrolling on that renders a growing horizontal
    // SCROLLBAR at the very bottom - it grows as the marquee scrolls, flickers where it crosses
    // the astronaut GIF (~3/4), then resets with the marquee: the "line at the bottom". The
    // big-clock screen (scr1) already clears this; the main screen did not.
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // 6068 - weather icon, initial src = first table entry (code 1, 64x64).
    iconImg = lv_img_create(screen);
    lv_img_set_src(iconImg, LV_WX_ICONS[0]);

    // 6058 - condition badge: pos(168,80) size(72,25), bg palette(5).
    condBadge = lv_obj_create(screen);
    lv_obj_add_style(condBadge, &stBadge, 0);
    lv_obj_clear_flag(condBadge, LV_OBJ_FLAG_SCROLLABLE);      // no scrollbar line
    lv_obj_set_pos(condBadge, 168, 80);
    lv_obj_set_size(condBadge, 72, 25);
    lv_obj_set_style_bg_color(condBadge, Lv::c565(PAL_5), 0);
    // 6064 - its label, "%s" with "Sunny".
    condLbl = lv_label_create(condBadge);
    lv_obj_add_style(condLbl, &stBody, 0);
    lv_obj_set_style_bg_opa(condLbl, LV_OPA_TRANSP, 0);        // label bg fully transparent
    lv_label_set_text_fmt(condLbl, "%s", "Sunny");

    // 6060 - city, recolor ON, "Beijing".
    cityLbl = lv_label_create(screen);
    lv_obj_add_style(cityLbl, &stBody, 0);
    lv_label_set_recolor(cityLbl, true);
    // Pre-fetch placeholder only - overwritten by the first successful fetch.
    lv_label_set_text(cityLbl, "Durban");

    // 605c - country badge: pos(120,15) size(50,25), bg palette(14), opa 0 at build.
    countryBadge = lv_obj_create(screen);
    lv_obj_add_style(countryBadge, &stBadge, 0);
    lv_obj_clear_flag(countryBadge, LV_OBJ_FLAG_SCROLLABLE);   // no scrollbar line
    lv_obj_set_pos(countryBadge, 120, 15);
    lv_obj_set_size(countryBadge, 50, 25);
    lv_obj_set_style_bg_color(countryBadge, Lv::c565(PAL_14), 0);
    // 6054 - its label, default "CN".
    countryLbl = lv_label_create(countryBadge);
    lv_obj_add_style(countryLbl, &stBody, 0);
    lv_obj_set_style_bg_opa(countryLbl, LV_OPA_TRANSP, 0);     // label bg fully transparent
    lv_label_set_text(countryLbl, "CN");
    lv_obj_set_style_opa(countryBadge, 0, 0);                  // prop 0x60

    // 6050 - the detail marquee. set_text("") then the placeholder fmt with
    // (0, 0, "0m/s", 1000); recolor ON; OVERFLOW_VISIBLE (flag 0x40000).
    detailLbl = lv_label_create(screen);
    lv_obj_add_style(detailLbl, &stBody, 0);
    lv_label_set_text(detailLbl, "");
    lv_label_set_text_fmt(detailLbl, FMT_DETAIL_PH, 0, 0, "0m/s", 1000);
    lv_label_set_recolor(detailLbl, true);
    lv_obj_add_flag(detailLbl, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // The marquee anim. Endpoints are computed ONCE from the PLACEHOLDER text width and
    // never recomputed when real data lands: start = screenWidth - 100 = 140,
    // end = -20 - labelWidth, 16000 ms, infinite, linear.
    lv_obj_update_layout(screen);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, detailLbl);
    lv_anim_set_exec_cb(&a, marqueeExec);
    lv_anim_set_values(&a, lv_obj_get_width(screen) - 100,
                           -20 - (int32_t)lv_obj_get_width(detailLbl));
    lv_anim_set_time(&a, 16000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);   // LVGL's name for 0xFFFF
    lv_anim_start(&a);

    // 604c/6048 - hour digits, "%01d" 0 (PLAIN; colour arrives via FMT_DIGIT on tick).
    hourT = lv_label_create(screen);
    lv_obj_add_style(hourT, &stClock, 0);
    lv_label_set_recolor(hourT, true);
    lv_label_set_text_fmt(hourT, FMT_DIGIT_PLAIN, 0);
    hourU = lv_label_create(screen);
    lv_obj_add_style(hourU, &stClock, 0);
    lv_label_set_recolor(hourU, true);
    lv_label_set_text_fmt(hourU, FMT_DIGIT_PLAIN, 0);
    // 6044 - separator, "%s" ":".
    sepLbl = lv_label_create(screen);
    lv_obj_add_style(sepLbl, &stClock, 0);
    lv_label_set_recolor(sepLbl, true);
    lv_label_set_text_fmt(sepLbl, "%s", ":");
    // 6040/603c - minute digits, created "#ffa500 %01d#" and recoloured #FF5A00 later.
    minT = lv_label_create(screen);
    lv_obj_add_style(minT, &stClock, 0);
    lv_label_set_recolor(minT, true);
    lv_label_set_text_fmt(minT, FMT_DIGIT_ORANGE, 0);
    minU = lv_label_create(screen);
    lv_obj_add_style(minU, &stClock, 0);
    lv_label_set_recolor(minU, true);
    lv_label_set_text_fmt(minU, FMT_DIGIT_ORANGE, 0);

    // 6038 - the seconds field, "%02d" 0, text_align CENTER (prop 0x5b = 2).
    secLbl = lv_label_create(screen);
    lv_obj_add_style(secLbl, &stLead, 0);
    lv_label_set_recolor(secLbl, true);
    lv_label_set_text_fmt(secLbl, FMT_SECONDS_PH, 0);
    lv_obj_set_style_text_align(secLbl, LV_TEXT_ALIGN_CENTER, 0);

    // 6034 - date, "%2d/%2d/%d %s" with (10, 24, 2023, "Mon"), recolor ON.
    dateLbl = lv_label_create(screen);
    lv_obj_add_style(dateLbl, &stBody, 0);
    lv_label_set_text_fmt(dateLbl, FMT_DATE_PH, 10, 24, 2023, "Mon");
    lv_label_set_recolor(dateLbl, true);

    // 6030/602c/6028 - temperature gauge row: icon baked at final size (see note above),
    // bar range(-50,50) size(60,12) indicator palette(0) value 10, label "%2d deg C" 18.
    gaugeTempImg = lv_img_create(screen);
    lv_img_set_src(gaugeTempImg, &IMG_WX_GAUGE_TEMP);
    tempBar = lv_bar_create(screen);
    lv_obj_add_style(tempBar, &stBar, 0);
    lv_bar_set_range(tempBar, -50, 50);
    lv_obj_set_size(tempBar, 60, 12);
    lv_obj_set_style_bg_color(tempBar, Lv::c565(PAL_0), LV_PART_INDICATOR);
    lv_bar_set_value(tempBar, 10, LV_ANIM_ON);
    tempLbl = lv_label_create(screen);
    lv_obj_add_style(tempLbl, &stBody, 0);
    lv_label_set_text_fmt(tempLbl, FMT_TEMP_PH, 18);

    // 6024/6020/601c - humidity gauge row: icon baked at final size (see note above),
    // bar range(0,100) indicator palette(5) value 49, label the literal "50%%".
    gaugeHumiImg = lv_img_create(screen);
    lv_img_set_src(gaugeHumiImg, &IMG_WX_GAUGE_HUMI);
    humiBar = lv_bar_create(screen);
    lv_obj_add_style(humiBar, &stBar, 0);
    lv_bar_set_range(humiBar, 0, 100);
    lv_obj_set_size(humiBar, 60, 12);
    lv_obj_set_style_bg_color(humiBar, Lv::c565(PAL_5), LV_PART_INDICATOR);
    lv_bar_set_value(humiBar, 49, LV_ANIM_ON);
    humiLbl = lv_label_create(screen);
    lv_obj_add_style(humiLbl, &stBody, 0);
    lv_label_set_text(humiLbl, TXT_HUMI_PH);

    // The closing align block - 18 calls, order matters.
    lv_obj_align(iconImg,      LV_ALIGN_TOP_RIGHT,  -3,   5);
    // These two are CENTER, not TOP_MID(0,-9)/(0,-8). Those offsets only centre the label
    // when the badge carries a `card` pad_top of exactly dpx(16); LVGL 8.3.11 at the default
    // DPI computes a larger PAD_DEF, which dropped the text ~5 px too LOW (seen on camera).
    // Centring in the badge directly is correct whatever the theme pad turns out to be.
    lv_obj_align(condLbl,      LV_ALIGN_CENTER,      0,   0);
    lv_obj_align(countryLbl,   LV_ALIGN_CENTER,      0,   0);
    lv_obj_align(cityLbl,      LV_ALIGN_TOP_LEFT,   10,  30);
    lv_obj_align(detailLbl,    LV_ALIGN_TOP_LEFT,    0,  52);
    // x chosen so the two icons' centres land within half a pixel of each other (temp
    // 12+17/2=20.5, humi 9+22/2=20) now that both are baked at final size with no runtime
    // zoom pivot to fight - see the note above gaugeTempImg/gaugeHumiImg's creation.
    lv_obj_align(gaugeTempImg, LV_ALIGN_LEFT_MID,   12,  70);
    lv_obj_align(tempBar,      LV_ALIGN_LEFT_MID,   35,  70);
    lv_obj_align(tempLbl,      LV_ALIGN_LEFT_MID,  103,  70);
    lv_obj_align(gaugeHumiImg, LV_ALIGN_LEFT_MID,    9, 100);
    lv_obj_align(humiBar,      LV_ALIGN_LEFT_MID,   35, 100);
    lv_obj_align(humiLbl,      LV_ALIGN_LEFT_MID,  103, 100);
    lv_obj_align(hourT,        LV_ALIGN_LEFT_MID,    5, -10);
    lv_obj_align(hourU,        LV_ALIGN_LEFT_MID,   40, -10);
    lv_obj_align(sepLbl,       LV_ALIGN_LEFT_MID,   78, -12);
    lv_obj_align(minT,         LV_ALIGN_LEFT_MID,   95, -10);
    lv_obj_align(minU,         LV_ALIGN_LEFT_MID,  130, -10);
    lv_obj_align(secLbl,       LV_ALIGN_LEFT_MID,  185,   5);
    lv_obj_align(dateLbl,      LV_ALIGN_LEFT_MID,   10,  40);

    Lv::adoptScreen(screen, &screen);
    Lv::logMem("weather");
}

// City/nickname validity: non-empty, not "0" and not "NULL".
inline bool validName(const char* s) {
    return s && s[0] && strcmp(s, "0") != 0 && strcmp(s, "NULL") != 0;
}

// The data half. All value parameters are PRE-FORMATTED strings, formatted at fetch time.
// `city` is the fetched name and `nickname` the user override; note that BOTH the x-position
// rule and the country-badge visibility key on strlen(city), not on the displayed string.
inline void updateMain(const char* city, const char* nickname, const char* country,
                       const char* cond, int iconCode,
                       const char* vMin, const char* vMax, const char* vFeels,
                       const char* vWind, const char* vAtm,
                       const char* vTemp, int tempVal, int humidity) {
    if (!screen) return;
    lv_label_set_text_fmt(detailLbl, FMT_DETAIL, vMin, vMax, vFeels, vWind, vAtm);

    const char* shown = validName(nickname) ? nickname
                      : validName(city)     ? city : "";
    lv_label_set_text(cityLbl, shown);
    lv_obj_align(cityLbl, LV_ALIGN_TOP_LEFT, strlen(city) < 7 ? 20 : 4, 15);

    lv_label_set_text(condLbl, cond);

    lv_opa_t opa = strlen(city) < 10 ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_obj_set_style_opa(countryBadge, opa, 0);
    lv_obj_set_style_opa(countryLbl, opa, 0);
    lv_label_set_text(countryLbl, country);

    lv_img_set_src(iconImg, iconFor(iconCode));

    lv_label_set_text_fmt(tempLbl, "%s", vTemp);
    lv_bar_set_value(tempBar, tempVal, LV_ANIM_ON);
    lv_bar_set_value(humiBar, humidity, LV_ANIM_ON);
    lv_label_set_text_fmt(humiLbl, FMT_HUMI, humidity);
}

// `sec` updates every call; the digits and date only when `minuteChanged`. Colour strings
// are the SHARED timecolor buffers - LvClock's, loaded from /.sys/timecolor.json.
// dateMode 1..5 selects format and argument order; anything else leaves the date.
inline void setClockAndDate(int sec, bool minuteChanged, int hh, int mm,
                            int dateMode, int wday0, int mday, int mon, int year) {
    if (!screen) return;
    lv_label_set_text_fmt(secLbl, FMT_SECONDS, LvClock::secCol, sec);
    if (!minuteChanged) return;
    lv_label_set_text_fmt(hourT, FMT_DIGIT, LvClock::hourCol, hh / 10);
    lv_label_set_text_fmt(hourU, FMT_DIGIT, LvClock::hourCol, hh % 10);
    lv_label_set_text_fmt(sepLbl, "%s", ":");
    lv_label_set_text_fmt(minT, FMT_DIGIT, LvClock::minCol, mm / 10);
    lv_label_set_text_fmt(minU, FMT_DIGIT, LvClock::minCol, mm % 10);
    const char* wd = WDAY[((wday0 % 7) + 7) % 7];
    switch (dateMode) {
        case 1: lv_label_set_text_fmt(dateLbl, FMT_DATE_1, mday, mon, year, wd); break;
        case 2: lv_label_set_text_fmt(dateLbl, FMT_DATE_2, year, mon, mday, wd); break;
        case 3: lv_label_set_text_fmt(dateLbl, FMT_DATE_3, mon, mday, year, wd); break;
        case 4: lv_label_set_text_fmt(dateLbl, FMT_DATE_45, mon, mday, wd);      break;
        case 5: lv_label_set_text_fmt(dateLbl, FMT_DATE_45, mday, mon, wd);      break;
        default: break;   // unknown mode leaves the date alone
    }
}

// -- Screen 1: the big digital clock ------------------------------------------------
// Every widget is lv_obj_set_pos + SIZE_CONTENT; fonts per label, no explicit text
// colour (the dark default theme's white - LV_THEME_DEFAULT_DARK 1 matches the
// original's dark=1 theme init). Placeholder state reads "22 deg  / Clear / 00:23".
inline void buildBig() {
    initStyles();
    if (scr1) { Lv::adoptScreen(scr1, &scr1); return; }
    s1Icon = s1Temp = s1HourT = s1Cond = s1MinT = s1Colon = s1HourU = s1MinU = nullptr;

    scr1 = lv_obj_create(nullptr);
    lv_obj_set_pos(scr1, 0, 0);
    lv_obj_set_size(scr1, 240, 240);
    lv_obj_clear_flag(scr1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr1, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr1, LV_OPA_COVER, 0);

    s1Icon = lv_img_create(scr1);
    lv_obj_set_pos(s1Icon, 33, 29);                       // (33, 29)
    lv_obj_set_size(s1Icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_img_set_src(s1Icon, LV_WX_ICONS[0]);

    s1Temp = lv_label_create(scr1);
    lv_obj_set_pos(s1Temp, 132, 29);                       // (132, 29)
    lv_obj_set_size(s1Temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1Temp, &FONT_TEMP52, 0);
    lv_label_set_text(s1Temp, "22\xC2\xB0");                  // bytes

    s1HourT = lv_label_create(scr1);
    lv_obj_set_pos(s1HourT, 32, 149);                      // (32, 149)
    lv_obj_set_size(s1HourT, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1HourT, &FONT_CLOCK62, 0);
    lv_label_set_text(s1HourT, "0");

    s1Cond = lv_label_create(scr1);
    lv_obj_set_pos(s1Cond, 127, 87);                       // (127, 87)
    lv_obj_set_size(s1Cond, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1Cond, &FONT_STK30, 0);
    lv_label_set_text(s1Cond, "Clear");

    s1MinT = lv_label_create(scr1);
    lv_obj_set_pos(s1MinT, 130, 149);                       // (130, 149)
    lv_obj_set_size(s1MinT, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1MinT, &FONT_CLOCK62, 0);
    lv_label_set_text(s1MinT, "2");

    s1Colon = lv_label_create(scr1);
    lv_obj_set_pos(s1Colon, 110, 149);                      // (110, 149)
    lv_obj_set_size(s1Colon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1Colon, &FONT_CLOCK62, 0);
    lv_label_set_text(s1Colon, ":");

    s1HourU = lv_label_create(scr1);
    lv_obj_set_pos(s1HourU, 73, 149);                      // (73, 149)
    lv_obj_set_size(s1HourU, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1HourU, &FONT_CLOCK62, 0);
    lv_label_set_text(s1HourU, "0");

    s1MinU = lv_label_create(scr1);
    lv_obj_set_pos(s1MinU, 171, 149);                       // (171, 149)
    lv_obj_set_size(s1MinU, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s1MinU, &FONT_CLOCK62, 0);
    lv_label_set_text(s1MinU, "3");

    Lv::adoptScreen(scr1, &scr1);
    Lv::logMem("weather1");
}

// Clamps each value into 0..99 and writes the four one-character digit labels.
inline void setBigDigits(int hh, int mm) {
    if (!scr1) return;
    if (hh > 99) hh = 99; if (hh < 0) hh = 0;
    if (mm > 99) mm = 99; if (mm < 0) mm = 0;
    char d[2] = { 0, 0 };
    d[0] = (char)('0' + hh / 10); lv_label_set_text(s1HourT, d);
    d[0] = (char)('0' + hh % 10); lv_label_set_text(s1HourU, d);
    d[0] = (char)('0' + mm / 10); lv_label_set_text(s1MinT, d);
    d[0] = (char)('0' + mm % 10); lv_label_set_text(s1MinU, d);
}

// Digit text colours from the shared timecolor buffers: hours from hourCol, minutes from
// minCol, colon fixed white. Skips the leading '#'.
inline void setBigColours() {
    if (!scr1) return;
    lv_color_t hc = lv_color_hex((uint32_t)strtoul(LvClock::hourCol + 1, nullptr, 16));
    lv_color_t mc = lv_color_hex((uint32_t)strtoul(LvClock::minCol + 1, nullptr, 16));
    lv_obj_set_style_text_color(s1HourT, hc, 0);
    lv_obj_set_style_text_color(s1HourU, hc, 0);
    lv_obj_set_style_text_color(s1MinT, mc, 0);
    lv_obj_set_style_text_color(s1MinU, mc, 0);
    lv_obj_set_style_text_color(s1Colon, lv_color_hex(0xFFFFFF), 0);
}

// `raw` is FILTERED to digits and '+'/'-' only, capped at 11 chars; a degree sign is
// appended when the result is shorter than 10. Two edge cases worth keeping straight:
// null/empty input keeps the "--" default, but non-empty input that filters to NOTHING
// yields the empty string.
inline void setBigIconTemp(int iconCode, const char* raw, const char* cond) {
    if (!scr1) return;
    lv_img_set_src(s1Icon, iconFor(iconCode));
    char buf[16] = "--";
    if (raw && raw[0]) {
        unsigned n = 0;
        for (const char* p = raw; *p && n < 11; ++p)
            if ((*p >= '0' && *p <= '9') || *p == '+' || *p == '-') buf[n++] = *p;
        buf[n] = 0;
        if (n == 0) buf[0] = 0;
        else if (n < 10) { buf[n] = '\xC2'; buf[n + 1] = '\xB0'; buf[n + 2] = 0; }
    }
    lv_label_set_text(s1Temp, buf);
    lv_label_set_text(s1Cond, cond ? cond : "");
}

} // namespace LvWeather
