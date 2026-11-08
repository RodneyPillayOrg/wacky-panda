#pragma once
#include "AppBase.h"
#include "../Config.h"
#include "../Display.h"
#include "../LvSettings.h"
#include "../WiFiMgr.h"
#include <Arduino.h>
#include <WiFi.h>

// Settings app.
//
// ONE STATIC SCREEN of six recolour-enabled labels: no pages, no paging, no periodic
// refresh, and no heap/uptime/RSSI/CPU/PSRAM/About view. An earlier 3-page version of this
// screen was removed. AppCtrl handles the long-press exit by leaving the app, so
// onLongPress() here is deliberately empty.
//
// Layout: all six labels are LV_ALIGN_BOTTOM_MID with negative Y on a 240 px screen,
// giving these absolute Y positions and per-line colours:
//   40  #ffa500  "SmolTV<TAB>Pro"      (literal 0x09 between the words)
//   70           "Version:  " + #ff0000 <ver>      (TWO spaces after the colon)
//   100          "WiFi Mode: " + #44cef6 AP|STA
//   130          "SSID: " + #ffa500 <ssid>
//   160          "IP: " + #ffd700 <ip>
//   210 (footer) "@sweetlilmre"
class AppSettings : public AppBase {
public:
    const char* name() const override { return APP_SETTINGS; }

    // Read the WiFi state and draw once. The screen is static - nothing refreshes it.
    void onEnter() override {
        const bool sta = !WiFiMgr::isAP();
        const String ssid = sta ? WiFi.SSID() : String(AP_SSID);
        const String ip   = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        LvSettings::show(sta, ssid.c_str(), ip.c_str(), LvSettings::VERSION);
    }

    // Nothing happens on touch except the exit, which AppCtrl handles. No page cycling.
    void onLongPress() override {}

    // Nothing to do per frame: the six labels are written once in onEnter and never touched
    // again, and lv_timer_handler is pumped centrally by AppCtrl::runLoop.
    bool loop() override { return true; }
};
