#pragma once
#include <WiFi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>     // the credential stash that carries WiFi across a filesystem OTA
#include "Config.h"
#include "Log.h"

// AP identity (AP_SSID) comes from Config.h - see the identity block there.
//
// Credentials live in exactly ONE place on flash: /.sys/config.json {"a":"ssid","p":"pass"}.
// This class owns writing it; BootConfig::loadWiFiConfig owns reading it at boot, and
// main.cpp owns the connect and AP-fallback sequence. There is no second credential file
// and no NVS fallback.
class WiFiMgr {
public:
    // The AP BIT, not an equality against WIFI_AP.
    //
    // Scanning enables STA alongside the running AP and leaves it enabled, so the mode becomes
    // WIFI_AP_STA for the rest of the session. An equality test then reads as "not in AP mode",
    // which is why the setup wizard's "/" started serving the console instead of the portal the
    // moment Save City kicked off its background scan. In connected mode there is no AP at all
    // (main.cpp never runs both), so the bit alone is the answer.
    static bool isAP() { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }

    // -- Credential carry-over across a filesystem OTA -------------------------------
    //
    // A filesystem image is written whole, so it takes /.sys with it - including the only copy
    // of the credentials. These two move them through NVS, which lives in its own partition.
    //
    // This is NOT the NVS fallback that loadWiFiConfig deliberately refuses. That refusal
    // exists so a device cannot silently connect using stale credentials nobody remembers
    // setting. The stash keeps that property: it is written only in the seconds before a
    // filesystem write, restored only when the fresh filesystem has no SSID of its own, and
    // ERASED as it is read - so it cannot survive into a second boot, and both halves log.
    static constexpr const char* NVS_NS = "smoltv";

    static bool stashCredentials() {
        DynamicJsonDocument doc(512);
        if (!FSUtil::readJson(PATH_CONFIG, doc)) return false;
        const String ssid = doc["a"] | "";
        const String pass = doc["p"] | "";
        if (ssid.isEmpty()) return false;
        Preferences prefs;
        if (!prefs.begin(NVS_NS, false)) return false;
        prefs.putString("stash_a", ssid);
        prefs.putString("stash_p", pass);
        prefs.end();
        LOGF("[wifi] credentials stashed in NVS for the filesystem write");
        return true;
    }

    // Called once at boot, before the credentials are read.
    static bool restoreStashedCredentials() {
        Preferences prefs;
        if (!prefs.begin(NVS_NS, false)) return false;
        const String ssid = prefs.getString("stash_a", "");
        const String pass = prefs.getString("stash_p", "");
        // Consumed unconditionally, even when it turns out not to be needed: a stash that
        // outlives one boot is precisely the stale-credential hazard this must not become.
        prefs.remove("stash_a");
        prefs.remove("stash_p");
        prefs.end();
        if (ssid.isEmpty()) return false;

        DynamicJsonDocument cur(512);
        if (FSUtil::readJson(PATH_CONFIG, cur) && String(cur["a"] | "").length()) {
            LOGF("[wifi] NVS stash discarded - the filesystem already has an SSID");
            return false;
        }
        saveCredentials(ssid, pass);
        LOGF("[wifi] credentials restored from the NVS stash");
        return true;
    }

    // Persist credentials to /.sys/config.json  (a = ssid, p = pass).
    // Does NOT call ESP.restart() - caller must schedule restart after sending HTTP response.
    static void saveCredentials(const String& ssid, const String& pass) {
        // Ensure /.sys directory exists
        if (!LittleFS.exists(SYS_DIR)) LittleFS.mkdir(SYS_DIR);
        DynamicJsonDocument doc(256);
        doc["a"] = ssid;
        doc["p"] = pass;
        FSUtil::writeJson(PATH_CONFIG, doc);
    }
};
