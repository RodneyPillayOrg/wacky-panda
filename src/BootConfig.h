/*
 * BootConfig - firmware startup helpers, called in order from main.cpp's setup().
 */
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Config.h"

// SSID as read from the credential FILES (never from NVS). Empty => no credentials
// configured => go straight to AP mode without ever calling WiFi.begin().
inline String g_wifiSsid;

// -- readConfig ------------------------------------------------------------------
// Deliberately empty. Every /.sys/ setting is read lazily at its point of use - device-level
// ones in Display.h and AppCtrl.h, per-app ones in each app's reloadConfig - so there is
// nothing to do up front. Kept as the named step in main.cpp's boot sequence.
inline void readConfig() {
}

// -- loadWiFiConfig ----------------------------------------------------------------
// Reads /.sys/delay.json -> stores the boot-delay countdown in *countdown, then triggers
// WiFi.begin().
// Credentials come from /.sys/config.json; there is no NVS fallback (see below).
// Returns: 0 on success, -1 if delay.json missing.
inline int loadWiFiConfig(int* countdown) {
    *countdown = 0;

    int ret = 0;
    if (!LittleFS.exists(PATH_DELAY)) {
        ret = -1;
    } else {
        DynamicJsonDocument doc(128);
        if (FSUtil::readJson(PATH_DELAY, doc)) {
            // readInt, NOT `doc["delay"] | 0`. /set stores every value as a JSON string, so
            // the file reads {"delay":"30"}. ArduinoJson's operator| falls back whenever
            // is<int>() is false, which a string always is, so the raw form silently returned
            // 0 and the countdown never ran at any setting.
            int secs = FSUtil::readInt(doc, "delay", 0);
            if (secs < 0) secs = 0;
            if (secs > BOOT_DELAY_MAX_SECS) {
                LOGF("[boot] delay %d clamped to %d", secs, BOOT_DELAY_MAX_SECS);
                secs = BOOT_DELAY_MAX_SECS;
            }
            *countdown = secs;
        }
    }

    // Credentials live in exactly ONE place: /.sys/config.json {"a":"ssid","p":"pass"},
    // written only by WiFiMgr::saveCredentials.
    // NO NVS fallback - stale NVS credentials would silently connect to an unreachable
    // network.  Without explicit file credentials the device MUST enter AP mode so the
    // user can configure WiFi through the captive portal.
    // NOTE: do NOT call WiFi.mode() here - WiFi isn't initialised yet; Arduino does it
    // lazily on the first WiFi.begin() call.
    DynamicJsonDocument cfgDoc(512);
    String ssid, pass;
    if (FSUtil::readJson(PATH_CONFIG, cfgDoc)) {
        ssid = cfgDoc["a"] | "";
        pass = cfgDoc["p"] | "";
    }
    // Publish the FILE-sourced SSID. main.cpp must use this, never WiFi.SSID(), which
    // returns the SDK/NVS-cached value and made factory reset appear not to work.
    g_wifiSsid = ssid;

    if (!ssid.isEmpty()) {
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
    // If ssid is empty, WiFi.begin() is NOT called -> WiFi stays disconnected ->
    // 15-second timeout fires -> AP fallback path in main.cpp runs.
    return ret;
}

// -- setupCaptivePortal --------------------------------------------------------------
// DNS redirect so any request on the AP network resolves to 192.168.4.1.
#include <DNSServer.h>
static DNSServer _dnsServer;

inline void setupCaptivePortal() {
    // Redirect all DNS queries to AP IP (captive portal pattern)
    _dnsServer.start(53, "*", WiFi.softAPIP());
}

// Call this from the web server's handleClient loop to process DNS
inline void captivePortalHandle() {
    _dnsServer.processNextRequest();
}

// -- readNightModeConfig -----------------------------------------------------------
// Reads /.sys/timebrt.json (time-based brightness / night mode).
inline void readNightModeConfig(bool* enabled, int* startHour, int* endHour, int* dimBright) {
    *enabled   = false;
    *startHour = 22;
    *endHour   = 7;
    *dimBright = 10;

    DynamicJsonDocument doc(128);
    if (FSUtil::readJson(PATH_TIMEBRT, doc)) {
        // Every timebrt.json value is a STRING; `doc[k] | def` returns def for a string, so
        // all fields must go through FSUtil::readInt (atoi). See AppCtrl::_loadNightMode.
        *enabled   = FSUtil::readInt(doc, "en", 0) != 0;
        *startHour = FSUtil::readInt(doc, "t1", 22);
        *endHour   = FSUtil::readInt(doc, "t2", 7);
        *dimBright = FSUtil::readInt(doc, "b2", 10);
    }
}
