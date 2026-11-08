#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_wifi.h>       // esp_wifi_restore() on factory reset
#include "Config.h"
#include "Log.h"
#include "Display.h"
#include "HttpParams.h"

// -- WebRoutes - server-agnostic request handling -------------------------------
// handleSetCore holds the /set config contract for every app (~320 lines). It lives HERE
// rather than beside a server for a hard technical reason: two HTTP server libraries cannot
// appear in the same translation
// unit. ESPAsyncWebServer declares HTTP_GET/HTTP_POST/... as a bitmask enum
// (WebRequestMethod), while esp_http_server pulls in nghttp's http_parser.h which declares
// the SAME names as a sequential enum (http_method) - including both is a hard compile
// error, and the values are incompatible (bitmask vs ordinal), so silencing the clash with
// ESPAsyncWebServer's WEBSERVER_H guard would break its method matching.
//
// So: this header depends on NO server. SyncWeb.cpp (esp_http_server, :80) includes it
// and adapts via ReqParams/SetResult; any future server would do the same.

// -- App-control hooks ---------------------------------------------------------
// The web UI must drive the REAL app controller (AppCtrl in main.cpp). Wiring the
// object in directly would be circular (AppCtrl -> Display -> ... ), and the older
// Scheduler class the handlers were written against is a stub, so /set?open_app=
// silently did nothing. main.cpp installs these hooks at boot instead.
using AppActivateFn = bool (*)(const String&);
using AppNameFn     = const char* (*)();
using AppPageFn     = void (*)(int);      // step active app's page: +1 fwd, -1 back
using AppReloadFn   = void (*)();         // signal the running app to reload its config
using AppEnterFn    = void (*)();         // long-press equivalent: enter shown app / exit to launcher
inline AppActivateFn _appActivateFn = nullptr;
inline AppNameFn     _appNameFn     = nullptr;
inline AppPageFn     _appPageFn     = nullptr;
inline AppReloadFn   _appReloadFn   = nullptr;
inline AppEnterFn    _appEnterFn    = nullptr;
inline void setAppControlHooks(AppActivateFn activate, AppNameFn name) {
    _appActivateFn = activate;
    _appNameFn     = name;
}
// /set?page= drives the ACTIVE app's page through the SAME onTapCounter path a physical tap
// uses. It backs the web UI's next/prev buttons, and lets automation exercise tap-advance
// with screenshots without touching the sensor.
inline void setAppPageHook(AppPageFn fn) { _appPageFn = fn; }
// Live-apply a web "Save ..." to the RUNNING app, so e.g. Save Time recolours the clock
// immediately. _handleSet calls this after writing the config files; the hook just flags
// AppCtrl, which reloads on the main task next iteration.
inline void setAppReloadHook(AppReloadFn fn) { _appReloadFn = fn; }
inline void setAppEnterHook(AppEnterFn fn) { _appEnterFn = fn; }

// tz.json "mode": 1 = MANUAL (use its own offset), 0 = AUTO (follow the weather city).
// The web UI and the setup portal both send the WORD, so a bare toInt() reads "manual" as 0.
inline int _tzModeToInt(const String& v) {
    return (v == "manual" || v == "1") ? 1 : 0;
}

inline SetResult handleSetCore(const ReqParams& P) {
    // Helper: merge one param into a JSON doc stored at path
    auto setKey = [&](const char* path, const char* key, const String& val) {
        DynamicJsonDocument doc(512);
        FSUtil::readJson(path, doc);
        doc[key] = val;
        FSUtil::writeJson(path, doc);
    };

    // -- Special actions ---------------------------------------------------
    if (P.has("reboot")) {
        // Spawn FIRST, return second. Returning before the xTaskCreate left it as
        // unreachable code: the endpoint answered "Rebooting..." and the device never
        // restarted, so the button looked wired up while doing nothing at all.
        xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(1000)); ESP.restart(); },
                    "set_reboot", 2048, nullptr, 1, nullptr);
        return SetResult{200, "text/plain", "Rebooting..."};
    }
    if (P.has("reset")) {
        // Factory reset: wipe /.sys/ and reboot. Same unreachable-code bug as reboot had.
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(500));
            // Remove known config files
            const char* paths[] = {
                PATH_CONFIG, PATH_BRT, PATH_TIMEBRT, PATH_NTP, PATH_TZ,
                PATH_CITY, PATH_KEY, PATH_UNIT, PATH_COIN2, PATH_STOCK2,
                PATH_MONITOR, PATH_ALBUM, PATH_GIF, PATH_SEP, PATH_CCOLOR,
                PATH_C_THEME, PATH_G_THEME, PATH_K, PATH_STK,
                // c_chart/st_chart were missing, so the Bitcoin chart type survived a
                // factory reset.
                PATH_C_CHART, PATH_ST_CHART,
                // The rest of the set:
                PATH_APP, PATH_CITY, PATH_HOUR12, PATH_DAY, PATH_TIMECOLOR,
                PATH_DELAY, PATH_BILI, PATH_CGK, PATH_CGKL, PATH_CG_RANGE,
                nullptr
            };
            for (int i = 0; paths[i]; i++) LittleFS.remove(paths[i]);
            // Belt-and-braces NVS wipe. Strictly it is redundant - WiFi.begin() is only
            // ever called with a non-empty FILE-sourced SSID, so the SDK is never told to
            // auto-connect from its own copy - but erasing that copy removes the failure
            // mode entirely.
            esp_wifi_restore();
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP.restart();
        }, "set_reset", 4096, nullptr, 1, nullptr);
        return SetResult{200, "text/plain", "Factory reset..."};
    }
    if (P.has("open_app")) {
        // Routes to the real AppCtrl via the hook.
        bool ok = _appActivateFn && _appActivateFn(P.get("open_app"));
        Serial.printf("[web] open_app=%s -> %s\n",
                      P.get("open_app").c_str(), ok ? "ok" : "FAILED");
        // The UI's send() helper treats ANY body that is not exactly "OK" as a
        // failure: `const ok = check ? check(t,r) : t.trim()==='OK';
        //           if(!r.ok||!ok) throw new Error(t)`. Returning "{}" therefore
        // produced 'Open failed: {}' even though the activation had succeeded.
        // Every other /set branch already answers "OK" - match it.
        if (ok) return SetResult{200, "text/plain", "OK"};
        else    return SetResult{404, "text/plain", "no such app"};
    }
    if (P.has("page")) {
        // page=-1 = left/back, page=1 = right/forward. Steps the ACTIVE app's page via
        // the same tap path (AppCtrl::pageStep -> onTapCounter). No-op in the launcher.
        int dir = P.get("page").toInt();
        if (_appPageFn) _appPageFn(dir < 0 ? -1 : 1);
        return SetResult{200, "text/plain", "OK"};
    }
    if (P.has("enter")) {
        // Emulates a long press: enter the shown app from the launcher, or exit the running
        // app back to the launcher. Was a stub that answered "{}" and did nothing.
        //
        // Deferred to the main task rather than run here. Entering an app calls onEnter(),
        // which allocates the direct-draw band and touches LVGL state; doing that from the
        // web task races the render loop. Same reasoning as requestReload().
        if (_appEnterFn) _appEnterFn();
        return SetResult{200, "text/plain", "OK"};
    }
    if (P.has("clear")) {
        // clear=gif -> delete all files in /image/80x80.gif/
        // clear=image -> delete all files in /image/
        String kind = P.get("clear");
        String dir = (kind == "gif") ? "/image/80x80.gif" : "/image";
        File root = LittleFS.open(dir);
        if (root && root.isDirectory()) {
            File f = root.openNextFile();
            while (f) {
                if (!f.isDirectory()) LittleFS.remove(String(f.name()));
                f = root.openNextFile();
            }
        }
        return SetResult{200, "text/plain", "OK"};
    }

    // -- Config writes - each param written to its file --------------------
    // Brightness
    // Brightness. The panel is ACTIVE-LOW (higher LEDC duty = dimmer) and the firmware
    // writes brt.json's value straight to ledcWrite(0, ...) with NO inversion. The web slider
    // is a human 0-100 "brightness %", and the UI reads it back as uiPct = 255 - raw, so /set
    // must store raw = 255 - uiPct: brt=90 -> {"brt":"165"}, brt=10 -> "245", brt=78 -> "177".
    // Storing the raw 0-100 and applying it directly is why 50% once looked near-max-bright
    // and the value never round-tripped with the UI.
    if (P.has("brt")) {
        int ui  = constrain(P.get("brt").toInt(), 0, 100);   // web slider 0-100
        int raw = 255 - ui;                                     // active-low PWM duty
        Display::setDayBrightness((uint8_t)raw);                // day level night mode reads
        setKey(PATH_BRT, "brt", String(raw));                   // brt.json stores RAW
    }
    // -- Boot theme + per-app enable flags -> app.json ---------------------
    // persistAppSettings() sends /set?theme=<idx>&app_bitcoin=0/1&...&app_settings=... .
    // The file holds {"theme":"<idx>","apps":{"bitcoin":<0/1>, ...9... }} and AppCtrl::sortApps
    // applies it. These were once DROPPED here, so the web app-enable toggles and the
    // startup-app selector did nothing. theme is a STRING.
    {
        static const char* const APPNAMES[9] = {"bitcoin","coingecko","stock","weather",
            "picture","monitor","clock","idea","settings"};
        bool any = P.has("theme");
        for (int i = 0; i < 9 && !any; i++)
            any = P.has((String("app_") + APPNAMES[i]).c_str());
        if (any) {
            DynamicJsonDocument doc(512); FSUtil::readJson(PATH_APP, doc);
            appJsonDropOrphanKeys(doc);                          // strip legacy rot_i/delay
            if (P.has("theme")) doc["theme"] = P.get("theme");   // stored as a string
            JsonObject apps = doc["apps"].is<JsonObject>()
                ? doc["apps"].as<JsonObject>() : doc.createNestedObject("apps");
            for (int i = 0; i < 9; i++) {
                String k = String("app_") + APPNAMES[i];
                if (P.has(k.c_str())) apps[APPNAMES[i]] = P.get(k.c_str()).toInt();
            }
            FSUtil::writeJson(PATH_APP, doc);
        }
    }
    // Night mode (timebrt.json). t1/t2 = start/end HOUR (0-23, as-is), en = enable.
    // b2 = night brightness, same active-low convention as brt: the UI sends 0-100 and
    // loadSettingsConfig reads it back as 255 - b2, so store raw = 255 - uiPct. b1 is a
    // hardcoded 50 the UI sends but the firmware never reads (NightMode uses only b2).
    // b1 is listed in the guard as well as assigned below. It was assigned but NOT guarded,
    // so a request carrying b1 alone never entered the block and was dropped in silence. The
    // page always batches b1 with b2/t1/t2/en, which hid it.
    if (P.has("t1") || P.has("t2") || P.has("b1") || P.has("b2") || P.has("en")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_TIMEBRT, doc);
        if (P.has("t1")) doc["t1"] = P.get("t1");
        if (P.has("t2")) doc["t2"] = P.get("t2");
        if (P.has("b1")) doc["b1"] = P.get("b1");
        if (P.has("b2")) doc["b2"] = String(255 - constrain(P.get("b2").toInt(), 0, 100));
        if (P.has("en")) doc["en"] = P.get("en");
        FSUtil::writeJson(PATH_TIMEBRT, doc);
    }
    // City / weather
    // cd2 likewise: assigned below but missing from this guard, so cd2 on its own was dropped.
    if (P.has("cd1") || P.has("cd2") || P.has("nickname") || P.has("interval")) {
        DynamicJsonDocument doc(512); FSUtil::readJson(PATH_CITY, doc);
        if (P.has("cd1"))      doc["cd1"]      = P.get("cd1");
        if (P.has("cd2"))      doc["cd2"]      = P.get("cd2");
        if (P.has("nickname")) doc["nickname"] = P.get("nickname");
        if (P.has("interval")) doc["w_i"]      = P.get("interval");
        FSUtil::writeJson(PATH_CITY, doc);
        // Kick off a BACKGROUND scan so step 3's SSID list is ready by the time
        // the user gets there - the portal says as much ("Saving city will also start a
        // Wi-Fi scan for step 3"). Async only: a blocking scan here would stall the request.
        //
        // The portal asks for /wifi.json immediately after this, so that handler must ADOPT
        // this scan rather than try to start its own - see h_wifiJson, where failing to do so
        // reported "No network found" on every first run.
        //
        // BOUNDED, with the same parameters h_wifiJson uses: async + show_hidden with the
        // default per-channel time can take up to 8 s, which outlasted the wait on the other
        // side and produced an empty list. 13 channels x 300 ms is about 4 s.
        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING)
            WiFi.scanNetworks(true /*async*/, SCAN_SHOW_HIDDEN, SCAN_PASSIVE,
                              SCAN_MS_PER_CHANNEL);
    }
    if (P.has("key"))  setKey(PATH_KEY,  "key",  P.get("key"));
    if (P.has("w_u") || P.has("t_u") || P.has("p_u")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_UNIT, doc);
        if (P.has("w_u")) doc["w_u"] = P.get("w_u");
        if (P.has("t_u")) doc["t_u"] = P.get("t_u");
        if (P.has("p_u")) doc["p_u"] = P.get("p_u");
        FSUtil::writeJson(PATH_UNIT, doc);
    }
    // Clock
    if (P.has("hc") || P.has("mc") || P.has("sc")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_TIMECOLOR, doc);
        if (P.has("hc")) doc["hc"] = P.get("hc");
        if (P.has("mc")) doc["mc"] = P.get("mc");
        if (P.has("sc")) doc["sc"] = P.get("sc");
        FSUtil::writeJson(PATH_TIMECOLOR, doc);
    }
    // 12/24h: the /set PARAM is "hour" but the hour12.json FILE key is "h" - that is what
    // AppClock reads and what the web UI reads back. Writing key "hour" here meant the toggle
    // never took effect and the page never showed it.
    if (P.has("hour")) setKey(PATH_HOUR12, "h", P.get("hour"));
    if (P.has("day"))  setKey(PATH_DAY,    "day",  P.get("day"));
    if (P.has("ntp"))  setKey(PATH_NTP,    "ntp",  P.get("ntp"));
    // tz.json keys are "mode" (compared == 1) and "offset" (MINUTES, clamped -720..840),
    // both NUMBERS: {"mode":1,"offset":120}.
    // Writing {"tz_mode":"manual","tz_offset":"120"} - wrong key names AND strings instead of
    // numbers - meant the offset was never applied and the clock ran two hours slow.
    if (P.has("tz_mode") || P.has("tz_offset") ||
        P.has("mode")    || P.has("offset")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_TZ, doc);
        // BOTH UIs send the WORD - "manual" or "auto" - and String::toInt() on either is 0,
        // which is AUTO. So every manual timezone pick was silently stored as auto and the
        // offset beside it ignored, leaving the clock on UTC. Accept the words and the digits.
        if (P.has("tz_mode")) doc["mode"] = _tzModeToInt(P.get("tz_mode"));
        if (P.has("mode"))    doc["mode"] = _tzModeToInt(P.get("mode"));
        if (P.has("tz_offset")) doc["offset"] = P.get("tz_offset").toInt();
        if (P.has("offset"))    doc["offset"] = P.get("offset").toInt();
        // readInt: usually this reads back the int just written above, but when neither
        // offset param is present it reads whatever the FILE held -- which a restore or a
        // hand edit could leave as a string, silently clamping a valid offset to 0.
        long off = FSUtil::readInt(doc, "offset", 0);
        if (off >  840) off =  840;      // +14 h
        if (off < -720) off = -720;
        doc["offset"] = off;
        doc.remove("tz_mode");
        doc.remove("tz_offset");
        FSUtil::writeJson(PATH_TZ, doc);
    }
    // -- Coin / BTC --------------------------------------------------------
    // Web UI saveBitcoin() sends THREE /set calls: c_theme, c_chart, kline (single),
    // then ONE batch of cin0..11 + cn0..11 + c_loading/c_clock/ani/c_pct_mode/c_i/c_l
    // (+ rot_i). File and key layout:
    //   coin2.json  fmt: {"c_loading":%d,"c_clock":%d,"c_ani":%d,
    //               "c_pct_mode":%d,"c_l":%d,"c_i":%d,"cn0..11":"%s","cin0..11":"%s"}
    //               NOTE the web param is "ani" but the FILE key is "c_ani".
    //   cr.json     {"rot_i":"%d"} - a QUOTED string.
    //   c_theme.json / c_chart.json / k.json("k"): singles.
    // Previously the whole batch was DROPPED (no handler -> silent "OK"), so a
    // web-configured BitCoin never persisted its symbols/interval/loop.
    if (P.has("c_theme")) setKey(PATH_C_THEME, "c_theme", P.get("c_theme"));
    if (P.has("c_chart")) setKey(PATH_C_CHART,  "c_chart", P.get("c_chart"));
    if (P.has("kline"))   setKey(PATH_K,        "k",       P.get("kline"));
    // The BitCoin batch is identified by cin0/c_i/c_pct_mode/c_l/ani/c_loading/c_clock
    // (the Stock app's batch uses c0../s_i and never sends these, so no cn* collision).
    if (P.has("cin0") || P.has("c_i") || P.has("c_pct_mode") ||
        P.has("c_l") || P.has("c_loading") || P.has("c_clock") ||
        P.has("ani")) {
        DynamicJsonDocument doc(1024); FSUtil::readJson(PATH_COIN2, doc);
        for (int i = 0; i < 12; i++) {
            String ck = String("cin") + i, nk = String("cn") + i;
            if (P.has(ck.c_str())) doc[ck] = P.get(ck.c_str());
            if (P.has(nk.c_str())) doc[nk] = P.get(nk.c_str());
        }
        if (P.has("c_loading"))  doc["c_loading"]  = P.get("c_loading").toInt();
        if (P.has("c_clock"))    doc["c_clock"]    = P.get("c_clock").toInt();
        if (P.has("ani"))        doc["c_ani"]      = P.get("ani").toInt();
        if (P.has("c_pct_mode")) doc["c_pct_mode"] = P.get("c_pct_mode").toInt();
        if (P.has("c_i"))        doc["c_i"]        = P.get("c_i").toInt();
        if (P.has("c_l"))        doc["c_l"]        = P.get("c_l").toInt();
        FSUtil::writeJson(PATH_COIN2, doc);
    }
    if (P.has("rot_i")) setKey(PATH_CR, "rot_i", P.get("rot_i"));  // cr.json, quoted
    // -- CoinGecko -----------------------------------------------------------
    // saveCoinGecko(): g_theme, g_range singles + batch gin0..11 + g_* to cgk.json.
    // cgk.json fmt: {"g_rot_i":%d,"g_loading":%d,"g_clock":%d,"g_ani":%d,
    //   "g_l":%d,"g_i":%d,"gin0..11":"%s"} - the g_* values are NUMERIC.
    if (P.has("g_theme")) setKey(PATH_G_THEME,  "g_theme", P.get("g_theme"));
    if (P.has("g_range")) setKey(PATH_CG_RANGE, "range",   P.get("g_range"));
    if (P.has("gin0") || P.has("g_i") || P.has("g_rot_i") ||
        P.has("g_l") || P.has("g_loading") || P.has("g_clock") ||
        P.has("g_ani")) {
        DynamicJsonDocument doc(1024); FSUtil::readJson(PATH_CGK, doc);
        for (int i = 0; i < 12; i++) {
            String gk = String("gin") + i;
            if (P.has(gk.c_str())) doc[gk] = P.get(gk.c_str());
        }
        if (P.has("g_rot_i"))   doc["g_rot_i"]   = P.get("g_rot_i").toInt();
        if (P.has("g_loading")) doc["g_loading"] = P.get("g_loading").toInt();
        if (P.has("g_clock"))   doc["g_clock"]   = P.get("g_clock").toInt();
        if (P.has("g_ani"))     doc["g_ani"]     = P.get("g_ani").toInt();
        if (P.has("g_l"))       doc["g_l"]       = P.get("g_l").toInt();
        if (P.has("g_i"))       doc["g_i"]       = P.get("g_i").toInt();
        FSUtil::writeJson(PATH_CGK, doc);
    }
    // -- Stock (shares) ----------------------------------------------------
    // saveStock(): st_chart, st_kline singles + batch s_i/s_l/s_ani + c0..9 + cn0..9.
    // stock2.json fmt: {"s_ani":"%d","s_l":"%d","s_i":"%d","cn0..9":"%s",
    //   "c0..9":"%s"} - the s_* values are QUOTED strings. Batch identified by
    //   c0/s_i/s_l/s_ani (BitCoin uses cin0/c_i and never sends these).
    if (P.has("st_chart")) setKey(PATH_ST_CHART, "st_chart", P.get("st_chart"));
    if (P.has("st_kline")) setKey(PATH_STK,      "st_kline", P.get("st_kline"));
    if (P.has("c0") || P.has("s_i") || P.has("s_l") ||
        P.has("s_ani")) {
        DynamicJsonDocument doc(1024); FSUtil::readJson(PATH_STOCK2, doc);
        for (int i = 0; i < 10; i++) {
            String ck = String("c") + i, nk = String("cn") + i;
            if (P.has(ck.c_str())) doc[ck] = P.get(ck.c_str());
            if (P.has(nk.c_str())) doc[nk] = P.get(nk.c_str());
        }
        if (P.has("s_ani")) doc["s_ani"] = P.get("s_ani");   // quoted
        if (P.has("s_l"))   doc["s_l"]   = P.get("s_l");
        if (P.has("s_i"))   doc["s_i"]   = P.get("s_i");
        FSUtil::writeJson(PATH_STOCK2, doc);
    }
    // Monitor
    if (P.has("ip") || P.has("m_i")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_MONITOR, doc);
        if (P.has("ip"))  doc["ip"]  = P.get("ip");
        if (P.has("m_i")) doc["m_i"] = P.get("m_i");
        FSUtil::writeJson(PATH_MONITOR, doc);
    }
    // Image / picture settings - all go to PATH_ALBUM
    if (P.has("gif_loop") || P.has("i_i") || P.has("autoplay") || P.has("album_path")
        || P.has("album_autoplay")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_ALBUM, doc);
        // gif_loop is stored as a STRING, autoplay as an INT:
        //   {"gif_loop":"2","i_i":"5", "autoplay":1, "path":"/image/..."}
        // i.e. gif_loop/i_i/path are strings and autoplay alone is numeric. Writing gif_loop
        // as an int broke nothing observable - the reader is the string-tolerant
        // FSUtil::readInt and the UI coerces with String() - but the types are part of the
        // file's contract, so keep them.
        if (P.has("gif_loop"))   doc["gif_loop"]  = P.get("gif_loop");
        if (P.has("i_i"))        doc["i_i"]       = P.get("i_i");
        if (P.has("autoplay"))   doc["autoplay"]  = P.get("autoplay").toInt();
        if (P.has("album_path")) doc["path"]      = P.get("album_path");
        // Pinning one image sends album_path=<path>&album_autoplay=0 - a SECOND spelling of
        // the same album.json "autoplay" key, sent only by that action. Dropping it left the
        // slideshow running, so it advanced off the image the user had just pinned while the
        // save still answered OK.
        if (P.has("album_autoplay")) doc["autoplay"] = P.get("album_autoplay").toInt();
        FSUtil::writeJson(PATH_ALBUM, doc);
    }
    // Weather GIF selection - stored in PATH_GIF key "gif"
    if (P.has("gif")) setKey(PATH_GIF, "gif", P.get("gif"));
    // Global app delay
    if (P.has("delay")) setKey(PATH_DELAY, "delay", P.get("delay"));
    // Shared colour / separator
    // ccolor.json carries TEN keys, not four. Accepting only the first four is why the UI's
    // coin-name and percentage colours were SILENTLY DROPPED - it posts all ten. See
    // src/CoinPalette.h for the full table.
    // Document size 512, not 256: ten "#RRGGBB" keys overflow the smaller one.
    {
        static const char* kColorKeys[] = {
            "up_fg", "up_bg", "down_fg", "down_bg",
            "coin_fg", "coin_bg",
            "pct_up_fg", "pct_up_bg", "pct_down_fg", "pct_down_bg",
        };
        bool anyColor = false;
        for (const char* k : kColorKeys) if (P.has(k)) { anyColor = true; break; }
        if (anyColor) {
            DynamicJsonDocument doc(512); FSUtil::readJson(PATH_CCOLOR, doc);
            for (const char* k : kColorKeys) if (P.has(k)) doc[k] = P.get(k);
            FSUtil::writeJson(PATH_CCOLOR, doc);
        }
    }
    // Separators: the /set PARAM names are dec_sep/thous_sep, but the sep.json FILE keys are
    // decimal_sep/thousand_sep, which is what AppCoin reads. Translate.
    if (P.has("dec_sep") || P.has("thous_sep")) {
        DynamicJsonDocument doc(256); FSUtil::readJson(PATH_SEP, doc);
        if (P.has("dec_sep"))   doc["decimal_sep"]  = P.get("dec_sep");
        if (P.has("thous_sep")) doc["thousand_sep"] = P.get("thous_sep");
        FSUtil::writeJson(PATH_SEP, doc);
    }

    // Live-apply: signal the running app to reload its config, so a web "Save ..." takes
    // effect immediately - e.g. Save Time recolours the running clock.
    // The hook only sets a flag; AppCtrl reloads on the main task. Reached by every config
    // write path (the action branches - reboot/reset/open_app/page/clear - returned early).
    if (_appReloadFn) _appReloadFn();
    return SetResult{200, "text/plain", "OK"};
}
