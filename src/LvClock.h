#pragma once
#include <lvgl.h>
#include <time.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"

// -- The Clock screen --------------------------------------------------------------------
//
// Built once, then refreshed by a 1000 ms lv_timer created ONCE. The seconds ring is a real
// lv_arc, not the hand-computed geometry the pre-LVGL version drew.
namespace LvClock {

// Colour defaults, overlaid from /.sys/timecolor.json. LVGL label recolor markup is
// "#RRGGBB text#", so these are carried as STRINGS and embedded in the format.
inline char hourCol[10] = "#FFFFFF";
inline char minCol[10]  = "#FF5A00";
inline char secCol[10]  = "#FF5900";

// Weekday names, indexed (tm_wday).
inline const char* const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thur", "Fri", "Sat" };

inline lv_obj_t* screen  = nullptr;
inline lv_obj_t* arc     = nullptr;      // seconds arc
inline lv_obj_t* dateLbl = nullptr;
inline lv_obj_t* timeLbl = nullptr;
inline lv_obj_t* wdayLbl = nullptr;
inline lv_timer_t* tick  = nullptr;

inline int  dayFormat = 1;               // day.json "day"; 0 is coerced to 1
inline bool hour12    = false;           // hour12.json

inline void setDateText() {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    const int d = t.tm_mday, m = t.tm_mon + 1, y = t.tm_year + 1900;
    // The year is FOUR digits in every format - "26/7/2026", not "26/7/26" - and the
    // argument ORDER differs per format, so read each case carefully before editing.
    switch (dayFormat) {
        // 2 -> "%d/%d/%2d"  (year, month, mday)   YYYY/M/DD
        case 2:  lv_label_set_text_fmt(dateLbl, "#87cefa %d/%d/%2d#", y, m, d); break;
        // 3 -> "%d/%2d/%d"  (month, mday, year)   M/DD/YYYY
        case 3:  lv_label_set_text_fmt(dateLbl, "#87cefa %d/%2d/%d#", m, d, y); break;
        // 4 -> "%2d / %2d"  (month, mday)
        case 4:  lv_label_set_text_fmt(dateLbl, "#87cefa %2d / %2d#", m, d);    break;
        // 5 -> "%2d / %2d"  (mday, month) - same literal, swapped args
        case 5:  lv_label_set_text_fmt(dateLbl, "#87cefa %2d / %2d#", d, m);    break;
        // 1 -> "%2d/%d/%d"  (mday, month, year)   DD/M/YYYY
        default: lv_label_set_text_fmt(dateLbl, "#87cefa %2d/%d/%d#", d, m, y); break;
    }
}

inline void setTimeText() {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    // 24h: "%s %02d#:%s %02d#"     12h: "%s %d#:%s %02d#"
    // args are (hourColourString, hour, minuteColourString, minute).
    // THE COLON IS A LITERAL - it does not blink. Our pre-LVGL clock blinked it.
    if (hour12) {
        int h = t.tm_hour % 12;
        if (h == 0) h = 12;
        lv_label_set_text_fmt(timeLbl, "%s %d#:%s %02d#", hourCol, h, minCol, t.tm_min);
    } else {
        lv_label_set_text_fmt(timeLbl, "%s %02d#:%s %02d#", hourCol, t.tm_hour,
                              minCol, t.tm_min);
    }
}

inline void setWdayText() {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    lv_label_set_text(wdayLbl, WDAY[t.tm_wday % 7]);
}

inline void setArcSeconds() {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    // lv_arc_set_bg_angles(arc, 0, sec * 6). 6 degrees per second, so a full sweep per
    // minute, and with rotation 270 it starts at 12 o'clock.
    lv_arc_set_bg_angles(arc, 0, t.tm_sec * 6);
}

// The 1000 ms timer callback. Exactly 1 Hz.
inline void onTick(lv_timer_t*) {
    // The screen can be destroyed under us when another app takes over (adoptScreen nulls
    // this pointer). The timer keeps firing, so without this guard it would touch freed
    // widgets.
    if (!screen) return;
    setArcSeconds();
    setTimeText();
    setDateText();
    setWdayText();
}

inline void build() {
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    // Children died with any previously destroyed screen.
    arc = nullptr; dateLbl = nullptr; timeLbl = nullptr; wdayLbl = nullptr;

    // Container - EXACTLY these two calls, nothing more. Do NOT add radius, border_width or
    // padding: they were once added on the assumption that a "flat black container" needs
    // them, which changes what the theme would otherwise do. Same class of mistake as the
    // launcher's grey slab, below.
    screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);

    // -- SECONDS ARC -------------------------------------------------------------
    // w=0xE8(232) h=0xEA(234) - NOT square, and not the 232 both ways the old code used.
    // x=0, y=-1, align CENTER, value 100, rotation 0x10E(270) so 0 deg is 12 o'clock.
    arc = lv_arc_create(screen);
    lv_obj_set_width(arc, 232);
    lv_obj_set_height(arc, 234);
    lv_obj_set_x(arc, 0);
    lv_obj_set_y(arc, -1);
    lv_obj_set_align(arc, LV_ALIGN_CENTER);
    lv_arc_set_value(arc, 100);
    lv_arc_set_rotation(arc, 270);
    // INDICATOR: colour 0xF061 (a red), opa 255, width 2. The MAIN/track part is left
    // UNSTYLED, so it takes the dark theme's arc width LV_DPX(15) = 12 px and colour
    // 0x2986 - which is where those two values came from originally, and why the track is
    // thick while the indicator is a thin 2 px line.
    lv_obj_set_style_arc_color(arc, Lv::c565(0xF061), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 2, LV_PART_INDICATOR);
    // KNOB: bg white, opa 255.
    lv_obj_set_style_bg_color(arc, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);

    // -- DATE --------------------------------------------------------------------
    // LV_SIZE_CONTENT (0x27D1) both axes, x=0, y=61, align CENTER, recolor ON,
    // text_align CENTER (2), font = DATE22.
    dateLbl = lv_label_create(screen);
    lv_obj_set_width(dateLbl, LV_SIZE_CONTENT);
    lv_obj_set_height(dateLbl, LV_SIZE_CONTENT);
    lv_obj_set_x(dateLbl, 0);
    lv_obj_set_y(dateLbl, 61);
    lv_obj_set_align(dateLbl, LV_ALIGN_CENTER);
    lv_label_set_recolor(dateLbl, true);
    lv_obj_set_style_text_align(dateLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(dateLbl, &FONT_DATE22, 0);

    // -- TIME --------------------------------------------------------------------
    // LV_SIZE_CONTENT, x=0, y=1, align CENTER, recolor ON, font = TIME76.
    timeLbl = lv_label_create(screen);
    lv_obj_set_width(timeLbl, LV_SIZE_CONTENT);
    lv_obj_set_height(timeLbl, LV_SIZE_CONTENT);
    lv_obj_set_x(timeLbl, 0);
    lv_obj_set_y(timeLbl, 1);
    lv_obj_set_align(timeLbl, LV_ALIGN_CENTER);
    lv_label_set_recolor(timeLbl, true);
    lv_obj_set_style_text_font(timeLbl, &FONT_TIME76, 0);

    // -- WEEKDAY -----------------------------------------------------------------
    // w=0x61(97) h=0x17(23), x=1, y=-67, align CENTER, colour 0x07BF, opa 255,
    // text_align CENTER, font = WDAY25. Note x is 1, not 0.
    wdayLbl = lv_label_create(screen);
    lv_obj_set_width(wdayLbl, 97);
    lv_obj_set_height(wdayLbl, 23);
    lv_obj_set_x(wdayLbl, 1);
    lv_obj_set_y(wdayLbl, -67);
    lv_obj_set_align(wdayLbl, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(wdayLbl, Lv::c565(0x07BF), 0);
    lv_obj_set_style_text_opa(wdayLbl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(wdayLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wdayLbl, &FONT_WDAY25, 0);

    Lv::adoptScreen(screen, &screen);
    onTick(nullptr);
    Lv::logMem("clock");

    // 1000 ms, created ONCE - guarded on the timer pointer being null.
    if (!tick) tick = lv_timer_create(onTick, 1000, nullptr);
}

} // namespace LvClock
