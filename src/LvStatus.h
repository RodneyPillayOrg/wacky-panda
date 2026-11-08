#pragma once
#include "Config.h"
#include <lvgl.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"

// -- The boot / status screen, on LVGL -------------------------------------------
//
// Replaces Display::showStatus, which drew into the retired 38 KB sprite.
//
// Geometry and colours, all LV_ALIGN_TOP_MID with y from the top:
//     y=18   title "SmolTV PRO"     colour 0x86FF
//     y=54   88x88 spinner, arc w=8  track 0x19C9, indicator 0x6F7F
//     y=168  status line 1           colour 0xF7DF
//     y=198  status line 2           colour 0x7F9F
// The spinner is an lv_spinner: time 1000 ms, arc_length 80 degrees - so the 80-degree
// indicator completes one revolution per second. Under LVGL that is the widget's own job
// rather than an angle we compute per frame, which is also why it now animates smoothly
// during a blocking wait instead of only stepping when the text changes.
//
// This needs LV_USE_SPINNER in lv_conf.h. It was 0 for a while, because the pre-LVGL code
// drew the arc by hand.
namespace LvStatus {

inline lv_obj_t* screen  = nullptr;
inline lv_obj_t* title   = nullptr;
inline lv_obj_t* spinner = nullptr;
inline lv_obj_t* line1   = nullptr;
inline lv_obj_t* line2   = nullptr;

inline void begin() {
    if (screen) return;
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(screen);
    lv_label_set_text(title, DEVICE_NAME);
    lv_obj_set_style_text_font(title, &FONT_SMALL16, 0);
    // All four literals here are RGB565 words from the binary - see Lv::c565.
    lv_obj_set_style_text_color(title, Lv::c565(0x86FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

#if LV_USE_SPINNER
    spinner = lv_spinner_create(screen, 1000, 80);
    lv_obj_set_size(spinner, 88, 88);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, Lv::c565(0x19C9), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, Lv::c565(0x6F7F), LV_PART_INDICATOR);
#endif

    line1 = lv_label_create(screen);
    lv_label_set_text(line1, "");
    lv_obj_set_style_text_font(line1, &FONT_SMALL16, 0);
    lv_obj_set_style_text_color(line1, Lv::c565(0xF7DF), 0);
    lv_obj_align(line1, LV_ALIGN_TOP_MID, 0, 168);

    line2 = lv_label_create(screen);
    lv_label_set_text(line2, "");
    lv_obj_set_style_text_font(line2, &FONT_SMALL16, 0);
    lv_obj_set_style_text_color(line2, Lv::c565(0x7F9F), 0);
    lv_obj_align(line2, LV_ALIGN_TOP_MID, 0, 198);

    Lv::adoptScreen(screen, &screen);
    Lv::logMem("status");
}

inline void show(const char* l1, const char* l2 = "") {
    if (!screen) begin();
    if (lv_scr_act() != screen) Lv::adoptScreen(screen, &screen);
    lv_label_set_text(line1, l1 ? l1 : "");
    lv_label_set_text(line2, l2 ? l2 : "");
    lv_timer_handler();
}

// Hold a status screen for `ms` while still pumping LVGL, so the spinner keeps turning.
// The old equivalent drew one frame and then blocked, which made the spinner jerk forward
// only when the text changed.
inline void showFor(const char* l1, const char* l2, uint32_t ms) {
    show(l1, l2);
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        lv_timer_handler();
        delay(5);
    }
}

} // namespace LvStatus
