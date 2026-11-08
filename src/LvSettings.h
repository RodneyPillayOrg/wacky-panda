#pragma once
#include "Config.h"
#include <lvgl.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"

// -- The Settings screen ---------------------------------------------------------------
//
// ONE static page, six labels, ALL recolor-enabled, ALL aligned LV_ALIGN_BOTTOM_MID with a
// negative y. It is drawn ONCE and never refreshed: no paging, no touch handling, and the
// app's loop does nothing but service the exit flag.
namespace LvSettings {

// -- Styles ------------------------------------------------------------------------------
// container: bg_color 0 (black). That is the ONLY property it sets.
// title  : text_opa 0xFF, text_color 0xFFFFFFFF, font = PCT27
// body   : text_opa 0xFF, text_color 0xFFFFFFFF, font = PCT27
// footer : text_opa 0xFF, text_color 0xFFFFFFFF, font = BODY27
// NOTE the title and body styles are IDENTICAL - same font, same colour. Only the footer
// differs. Every visible colour on this screen comes from the recolor markup in the format
// strings below, never from a style.
inline lv_style_t stContainer, stTitle, stBody, stFooter;

inline lv_obj_t* screen  = nullptr;
inline lv_obj_t* lblTitle = nullptr;
inline lv_obj_t* lblVer   = nullptr;
inline lv_obj_t* lblMode  = nullptr;
inline lv_obj_t* lblSsid  = nullptr;
inline lv_obj_t* lblIp    = nullptr;
inline lv_obj_t* lblFoot  = nullptr;

// Format strings. Two details a re-typed version loses:
//   * the title has a literal TAB (0x09) between "SmolTV" and "Pro", not a space;
//   * "Version:" is followed by TWO spaces.
inline const char* const TXT_TITLE   = "#ffa500 SmolTV\tPro#";
inline const char* const TXT_VERSION = "Version:  #ff0000 %s#";
inline const char* const TXT_MODE_AP = "WiFi Mode: #44cef6 AP#";
inline const char* const TXT_MODE_STA= "WiFi Mode: #44cef6 STA#";
inline const char* const TXT_SSID    = "SSID: #ffa500 %s#";
inline const char* const TXT_IP      = "IP: #ffd700 %s#";
inline const char* const TXT_FOOTER  = "@sweetlilmre";

// The version string the screen renders.
inline const char* const VERSION = FW_VERSION;

inline void initStyles() {
    static bool done = false;
    if (done) return;
    done = true;
    lv_style_init(&stContainer);
    lv_style_set_bg_color(&stContainer, Lv::c565(0x0000));

    lv_style_init(&stTitle);
    lv_style_set_text_opa(&stTitle, LV_OPA_COVER);
    lv_style_set_text_color(&stTitle, lv_color_white());
    lv_style_set_text_font(&stTitle, &FONT_PCT27);

    lv_style_init(&stBody);
    lv_style_set_text_opa(&stBody, LV_OPA_COVER);
    lv_style_set_text_color(&stBody, lv_color_white());
    lv_style_set_text_font(&stBody, &FONT_PCT27);

    lv_style_init(&stFooter);
    lv_style_set_text_opa(&stFooter, LV_OPA_COVER);
    lv_style_set_text_color(&stFooter, lv_color_white());
    lv_style_set_text_font(&stFooter, &FONT_BODY27);
}

inline void build() {
    initStyles();
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    lblTitle = lblVer = lblMode = lblSsid = lblIp = lblFoot = nullptr;

    // Container: bg_color 0, add the container style, clear SCROLLABLE,
    // lv_obj_set_scrollbar_mode(OFF), then bg_opa 0 and border_width 0 on LV_PART_SCROLLBAR.
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0x0000), 0);
    lv_obj_add_style(screen, &stContainer, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(screen, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_border_width(screen, 0, LV_PART_SCROLLBAR);

    // Created in this order (title, mode, ssid, ip, footer, version), then styled, then
    // recolor-enabled, then aligned. Creation order matters only for z-order, but keep it.
    lblTitle = lv_label_create(screen);
    lblMode  = lv_label_create(screen);
    lblSsid  = lv_label_create(screen);
    lblIp    = lv_label_create(screen);
    lblFoot  = lv_label_create(screen);
    lblVer   = lv_label_create(screen);

    lv_obj_add_style(lblTitle, &stTitle,  0);
    lv_obj_add_style(lblMode,  &stBody,   0);
    lv_obj_add_style(lblSsid,  &stBody,   0);
    lv_obj_add_style(lblIp,    &stBody,   0);
    lv_obj_add_style(lblFoot,  &stFooter, 0);
    lv_obj_add_style(lblVer,   &stBody,   0);

    lv_label_set_recolor(lblTitle, true);
    lv_label_set_recolor(lblMode,  true);
    lv_label_set_recolor(lblSsid,  true);
    lv_label_set_recolor(lblIp,    true);
    lv_label_set_recolor(lblFoot,  true);
    lv_label_set_recolor(lblVer,   true);

    // All LV_ALIGN_BOTTOM_MID. The Version label is the only one with a non-zero x.
    lv_obj_align(lblTitle, LV_ALIGN_BOTTOM_MID, 0, -200);
    lv_obj_align(lblVer,   LV_ALIGN_BOTTOM_MID, 5, -170);
    lv_obj_align(lblMode,  LV_ALIGN_BOTTOM_MID, 0, -140);
    lv_obj_align(lblSsid,  LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_align(lblIp,    LV_ALIGN_BOTTOM_MID, 0,  -80);
    lv_obj_align(lblFoot,  LV_ALIGN_BOTTOM_MID, 0,  -30);

    Lv::adoptScreen(screen, &screen);
    Lv::logMem("settings");
}

// Writes all six labels. Called once, from onEnter.
inline void show(bool isSTA, const char* ssid, const char* ip, const char* version) {
    build();
    lv_label_set_text(lblTitle, TXT_TITLE);
    lv_label_set_text_fmt(lblVer, TXT_VERSION, version ? version : VERSION);
    lv_label_set_text(lblMode, isSTA ? TXT_MODE_STA : TXT_MODE_AP);
    lv_label_set_text_fmt(lblSsid, TXT_SSID, ssid ? ssid : "");
    lv_label_set_text_fmt(lblIp,   TXT_IP,   ip   ? ip   : "");
    lv_label_set_text(lblFoot, TXT_FOOTER);
}

} // namespace LvSettings
