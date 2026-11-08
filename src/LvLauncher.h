#pragma once
#include <lvgl.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"

// -- The launcher screen and its app-switch transition, 1:1 ----------------------
//
// Transcribed from three functions, every constant cited:
//   AppCtrl_initLauncherScreen   screen + both styles
//   AppCtrl_buildLauncherStatic   the no-animation build
//   AppCtrl_showTransition   the sliding build
//
// Nothing here is measured from a photo or taken from an LVGL default.
namespace LvLauncher {

// Slide extent. 0xB8 = 184 px, the X the icons animate from/to.
static constexpr int32_t SLIDE_X = 184;

// The 0x190 literal in showTransition is a SPEED, not a duration: it is passed to
// lv_anim_speed_to_time, which computes |start - end| * 1000 / speed. So the actual
// duration is 184 * 1000 / 400 = 460 ms. Mis-reading this literal as a time is exactly how
// "400 ms" got into the old implementation.
static constexpr uint32_t SLIDE_SPEED = 750;

// Direction codes as AppCtrl_runLoop passes them: 5 reverses the two SLIDE_X literals,
// 6 (or anything else) is forward, i.e. the new icon enters from the RIGHT.
static constexpr int DIR_REVERSE = 5;

inline lv_obj_t*  screen   = nullptr;
inline lv_obj_t*  child    = nullptr;
inline lv_style_t styleContainer;
inline lv_style_t styleLabel;

inline lv_obj_t*   curIcon  = nullptr;   // g_launcher_old_icon
inline lv_obj_t*   curLabel = nullptr;   // g_launcher_old_label
inline const void* curIconSrc = nullptr; // g_launcher_cur_icon_dsc

// AppCtrl_initLauncherScreen.
inline void init() {
    // Our screen may have been destroyed by adoptScreen when an app took over. The child
    // widgets died with it, so clear those pointers before rebuilding.
    if (!screen) { child = nullptr; curIcon = nullptr; curLabel = nullptr; curIconSrc = nullptr; }
    if (screen) return;
    // Container style: bg_color, radius and border_width, and nothing else.
    //
    // This was once written as border_width / pad_all / radius. Padding is not the third
    // property, and bg_color MUST be set to black: the default dark theme paints obj
    // backgrounds grey, so omitting it put a grey rounded slab behind the icon.
    lv_style_init(&styleContainer);
    lv_style_set_bg_color(&styleContainer, lv_color_black());
    lv_style_set_radius(&styleContainer, 0);
    lv_style_set_border_width(&styleContainer, 0);

    // Label style - PROVEN, and it is the whole reason the label's font could not be
    // guessed: the font comes from here, not from the label.
    //   helper(s, 0xff)              -> text_opa   255
    //   helper(s, 0xffffffff)        -> text_color white
    //   helper(s, &PTR_LAB_3f452568) -> text_font  the face,
    //                                         which is the one we extract as PCT27.
    lv_style_init(&styleLabel);
    lv_style_set_text_opa(&styleLabel, LV_OPA_COVER);
    lv_style_set_text_color(&styleLabel, lv_color_white());
    lv_style_set_text_font(&styleLabel, &FONT_PCT27);

    screen = lv_obj_create(nullptr);
    lv_obj_add_style(screen, &styleContainer, 0);
    // helper(screen, 0, 0x10000): 0x10000 is LV_PART_SCROLLBAR, value 0 -> hidden.
    lv_obj_set_style_opa(screen, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    child = lv_obj_create(screen);
    lv_obj_add_style(child, &styleContainer, 0);
    lv_obj_set_size(child, 240, 240);          // helper(child, 0xf0,...)
    lv_obj_align(child, LV_ALIGN_CENTER, 0, 0);
}

// Create the icon + label pair. Shared by both build paths because the geometry is THE SAME
// in both - the pre-LVGL code computed them separately and they disagreed.
//   icon:  lv_obj_align(LV_ALIGN_CENTER, 0, -20)
//   label: lv_obj_add_style(&styleLabel, 0), then
//          lv_obj_align_to(label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, +10)
inline void makePair(const void* iconSrc, const char* text,
                     lv_obj_t** outIcon, lv_obj_t** outLabel) {
    lv_obj_t* icon = lv_img_create(screen);
    lv_img_set_src(icon, iconSrc);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* label = lv_label_create(screen);
    lv_obj_add_style(label, &styleLabel, 0);
    lv_label_set_text(label, text);
    lv_obj_align_to(label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    *outIcon = icon;
    *outLabel = label;
}

// AppCtrl_buildLauncherStatic - the noAnim path.
inline void buildStatic(const void* iconSrc, const char* text) {
    init();     // our screen may have been destroyed by adoptScreen while an app ran
    if (curIcon)  { lv_obj_del(curIcon);  curIcon = nullptr; }
    if (curLabel) { lv_obj_del(curLabel); curLabel = nullptr; }
    curIconSrc = iconSrc;
    makePair(iconSrc, text, &curIcon, &curLabel);
    // Even the static path goes through a screen load: anim type NONE, time 300 ms.
    // adoptScreen destroys whatever screen was showing (an app's, or the boot screen), which
    // is what AppCtrl_initLauncherScreen does.
    //
    // THE SCREEN LOAD MUST BE UNCONDITIONAL. It ends with
    // `lv_scr_load_anim(screen, 0, 300)` and there must be NO guard on the screen already
    // being active: lv_disp_load_scr invalidates the screen it loads, and that invalidation
    // is what erases whatever was on the panel. A `!= screen` guard here skips it whenever
    // the outgoing app never lv_scr_load-ed a screen of its own - i.e.
    // the DIRECT-DRAW apps (Idea), which paint the panel through Display::renderApp while
    // LVGL still believes the launcher screen is displayed and unchanged. Deleting and
    // recreating the icon+label only invalidates those two small areas, so the rest of the
    // Idea animation stayed on the panel. Invalidate explicitly on the same-screen path,
    // which is exactly what the load would have done.
    if (lv_scr_act() != screen) Lv::adoptScreen(screen, &screen);
    else                        lv_obj_invalidate(screen);
    Lv::logMem("launcher");
}

// AppCtrl_showTransition - the sliding path.
//
// Two anims, BOTH driving an icon's X via lv_obj_set_x, path lv_anim_path_linear.
// Defaults (dir 6 = forward): new icon +SLIDE_X -> 0 (enters from the RIGHT),
// old icon 0 -> -SLIDE_X. dir == 5 swaps the two literals (new icon enters from the
// LEFT) - literal order read from the trace: newIconStartX = 0xb8 / oldIconEndX =
// 0xffffff48 on the non-5 path. THE LABEL DOES NOT ANIMATE: align_to is one-shot, so it
// swaps instantly and stays put, and the OLD label is deleted at creation time. Only two
// widgets are ever created - there is NO page indicator.
// Finally it BLOCKS until the anims finish, pumping lv_timer_handler.
inline void showTransition(const void* iconSrc, const char* text, int dir, bool noAnim) {
    if (noAnim) { buildStatic(iconSrc, text); return; }
    // Rebuild the screen if an app destroyed it, and make sure it is the one on display
    // before creating widgets on it. Without this, makePair() would be called with
    // screen == nullptr - and lv_img_create(nullptr) creates a SCREEN, not a child.
    init();
    if (lv_scr_act() != screen) Lv::adoptScreen(screen, &screen);
    if (curIconSrc == iconSrc) return;      // guarded on (!= iconDsc)

    int32_t newFrom = SLIDE_X, oldTo = -SLIDE_X;
    if (dir == DIR_REVERSE) { newFrom = -SLIDE_X; oldTo = SLIDE_X; }

    lv_obj_t* oldIcon  = curIcon;
    lv_obj_t* oldLabel = curLabel;

    curIconSrc = iconSrc;
    makePair(iconSrc, text, &curIcon, &curLabel);
    // The old label goes NOW, not when the animation ends - it never animates.
    if (oldLabel) lv_obj_del(oldLabel);

    auto setX = [](void* obj, int32_t v) { lv_obj_set_x((lv_obj_t*)obj, v); };

    lv_anim_t aNew;
    lv_anim_init(&aNew);
    lv_anim_set_var(&aNew, curIcon);
    lv_anim_set_exec_cb(&aNew, setX);
    lv_anim_set_values(&aNew, newFrom, 0);
    lv_anim_set_time(&aNew, lv_anim_speed_to_time(SLIDE_SPEED, newFrom, 0));
    lv_anim_set_path_cb(&aNew, lv_anim_path_linear);
    lv_anim_start(&aNew);

    if (oldIcon) {
        lv_anim_t aOld;
        lv_anim_init(&aOld);
        lv_anim_set_var(&aOld, oldIcon);
        lv_anim_set_exec_cb(&aOld, setX);
        lv_anim_set_values(&aOld, 0, oldTo);
        lv_anim_set_time(&aOld, lv_anim_speed_to_time(SLIDE_SPEED, 0, oldTo));
        lv_anim_set_path_cb(&aOld, lv_anim_path_linear);
        lv_anim_start(&aOld);
    }

    // Deliberately blocking: pump LVGL until the slide animation has finished.
    while (lv_anim_count_running()) lv_timer_handler();
    lv_timer_handler();

    if (oldIcon) lv_obj_del(oldIcon);
}

// Drive LVGL once. The launcher is the only LVGL surface for now, so this is called
// from the launcher paths only -- see the note in AppCtrl::_lvInit about why LVGL
// must not be pumped while an un-ported app owns the panel.
inline void pump() { lv_timer_handler(); }

} // namespace LvLauncher
