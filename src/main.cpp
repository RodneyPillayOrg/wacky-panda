/*
 * SmolTV Pro - firmware entry point.
 *
 * Boot sequence:
 *   1. TFT + LEDC backlight init
 *   2. Allocate AppController, LittleFS.begin("/littlefs", "spiffs")
 *   3. readConfig() - load all /.sys/ settings
 *   4. Ensure /image/ and /.sys/ directories exist
 *   5. Display FW_VERSION, then "Booting..."
 *   6. loadWiFiConfig() -> optional countdown delay
 *   7. Boot-loop guard (/r1.flag, /r2.flag)
 *   8. WiFi connect loop (AP fallback at 192.168.4.1, SSID AP_SSID)
 *   9. SyncWeb::begin() on :80, in both the connected and AP-mode paths
 *  10. NTP sync (up to 20 s, retry every 12 s) when connected
 *  11. readNightModeConfig()
 *  12. Touch::begin() [after the "Don't Touch Top" message]
 *  13. AppCtrl::addApp x 9, then AppCtrl::runLoop() - never returns
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>    // esp_ota_get_running_partition() - OTA slot diagnostics
#include <time.h>

#include "Config.h"
#include "Log.h"
#include <esp_system.h>
#include "Display.h"
// LVGL is now the renderer, not an opt-in experiment - these must NOT sit behind
// LVGL_SMOKE. They did, left over from the smoke-test patch, which is why the normal
// build could not see LvStatus even though the include line was right there.
#include "LvglDisplay.h"
#include "LvStatus.h"
#include "LvFontsGen.h"
#include "LvLauncher.h"
#include "LvImagesGen.h"
#include "Touch.h"
#include "Sound.h"
#include "BootConfig.h"     // readConfig, loadWiFiConfig, setupCaptivePortal, readNightModeConfig
#include "SyncWeb.h"        // the bomb-proof synchronous esp_http_server on :80 (whole UI)
#include "WebRoutes.h"     // setAppControlHooks/... - server-agnostic by design.
#include "apps/AppBase.h"
#include "apps/AppSlots.h"
#include "apps/AppSettings.h"

// -- App controller ----------------------------------------------------------------
#include "AppCtrl.h"

#define BLK8(tag) LOGF("[blk8] %-12s free=%u blk8=%u", tag, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))

static AppCtrl  g_ctrl;

// App instances - registration order matches launcher order
static AppSlots     appSlots;       // "Slots"
static AppSettings  appSettings;    // "Settings"

// -- Helpers -------------------------------------------------------------------------
// Route the web UI's app-control endpoints (/set?open_app=, /.sys/app.json) to the
// real controller. Without this they hit a stub Scheduler and silently did nothing.
// Original firmware equivalent: AppCtrl_activateByName.
static void installAppControlHooks() {
    setAppControlHooks(
        [](const String& name) -> bool { return g_ctrl.activateByName(name); },
        []() -> const char* { return g_ctrl.currentName(); });
    // /set?page= -> step the active app's page (same path as a physical tap).
    setAppPageHook([](int dir) { g_ctrl.pageStep(dir); });
    // Web "Save ..." -> live-reload the running app's config, e.g. Save Time recolours the
    // running clock.
    setAppReloadHook([]() { g_ctrl.requestReload(); });
    setAppEnterHook([]() { g_ctrl.requestEnterToggle(); });
}

// One frame of the boot/status screen. Geometry, colours and the 1000 ms / 80-degree spinner
// all live in LvStatus.
static void displayStatus(const char* msg, const char* line2 = "") {
    LvStatus::show(msg, line2);
}

// Hold a status screen for `ms` while ANIMATING the spinner. Use this instead of
// `displayStatus(x); delay(x);` - the old pattern drew a single frame and then blocked,
// so the spinner only jerked forward when the text changed instead of spinning smoothly.
static void displayStatusFor(const char* msg, uint32_t ms, const char* line2 = "") {
    LvStatus::showFor(msg, line2, ms);
}

// -- Loop task stack size -----------------------------------------------------
// The Arduino core creates loopTask with getArduinoLoopTaskStackSize(), which is
// weak, so overriding it here is the sanctioned way to enlarge it
// (cores/esp32/main.cpp:36,71 - default ARDUINO_LOOP_STACK_SIZE 8192).
//
// WHY: the Stock app crash-looped with reset_reason=4 (ESP_RST_PANIC) the instant it
// called http.GET() on fc.yahoo.com. Heap was never the problem (minfree 224 KB);
// uxTaskGetStackHighWaterMark() reported only 2524 BYTES of stack left at that
// point, and an mbedTLS handshake needs several KB. Our banded render path is what
// consumes the rest before the fetch is reached.
//
// The alternative is to leave the Arduino default of 8192 and move the Stock chart fetch
// onto a PERSISTENT ASYNCHRONOUS client, so the TLS work happens on another task and the app
// loop never carries a handshake on top of a render. This fetch is synchronous, so it
// needs the headroom. Sized to 16 KB, not tuned - RAM is at 22%.
size_t getArduinoLoopTaskStackSize(void) { return 16384; }

void setup() {
#ifdef POWER_LATCH_PIN
    // Spotpear ESP32-S3 1.54" MUMA battery power hold pin (GPIO 3 HIGH)
    pinMode(POWER_LATCH_PIN, OUTPUT);
    digitalWrite(POWER_LATCH_PIN, HIGH);
#endif
    Serial.begin(115200);
    delay(200);

    Serial.println("[boot] setup start");
    // FW_VERSION is bumped on every change, so this line proves WHICH image booted. The
    // __DATE__/__TIME__ stamp cannot: it only moves when main.cpp itself is recompiled.
    LOGF("[boot] " FW_VERSION " (" __DATE__ " " __TIME__ ")");
    // Reset reason turns "it rebooted" into a diagnosis. esp_reset_reason_t:
    //   1 = POWERON, 2 = EXT, 3 = SW (esp_restart), 4 = PANIC (crash/abort),
    //   5 = INT_WDT, 6 = TASK_WDT, 7 = WDT, 8 = DEEPSLEEP, 9 = BROWNOUT.
    LOGF("[boot] reset_reason=%d minfree=%u", (int)esp_reset_reason(),
         (unsigned)ESP.getMinFreeHeap());
    // blk8 AT BOOT, before WiFi/the web server/any app has allocated anything. If it is
    // already ~30 KB here then the ceiling is STATIC (.bss crowding a DRAM region) and no
    // amount of runtime tidying will help; if it is ~45 KB here and ~30 KB later, it is
    // runtime fragmentation. That distinction decides how #41 gets fixed, so measure it
    // rather than reasoning about the linker map.
    LOGF("[boot] blk8=%u blkdef=%u free8=%u",
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    // Which OTA slot are we running from? Needed to verify that an OTA actually took
    // effect (a successful /update returns 200 and reboots, but without this you cannot
    // tell whether the new image is the one running).
    {
        const esp_partition_t* run = esp_ota_get_running_partition();
        if (run) Serial.printf("[boot] running partition: %s @0x%06x size=0x%06x\n",
                               run->label, (unsigned)run->address, (unsigned)run->size);
    }
    Serial.printf("[heap] boot: free=%u max=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // Disable WiFi NVS auto-reconnect so stale credentials cannot bypass
    // our explicit credential-file logic later.
    WiFi.persistent(false);
    WiFi.disconnect(true);   // stop any in-progress reconnect, erase NVS creds

    // (The vfs_api "does not exist" spam from absent config files is suppressed with
    //  -DCORE_DEBUG_LEVEL=0 in platformio.ini - it is ARDUHAL's log_e, so neither
    //  esp_log_level_set() nor an exists() pre-check silences it. See the note there.)

    // 1. Display hardware init (TFT + backlight, NO sprite yet).
    Display::begin();
    BLK8("display");
    // LVGL comes up HERE, right after the panel, exactly as display_boot_task
    // does it (TFT init -> lv_init -> draw buf -> drv register). It used
    // to be brought up lazily on the first launcher draw, by which time the old
    // renderer had already taken 38 KB of heap for a sprite that then could not
    // coexist with it.
    Lv::begin(Display::tft);
    LvStatus::begin();
    BLK8("lvgl");
#if LVGL_SMOKE
    // LVGL SMOKE TEST - build with -DLVGL_SMOKE=1 only.
    //
    // Purpose: prove that include/lv_conf.h is actually in force, that the static
    // 240x80 draw buffer and the DMA flush transcribed from lvgl_disp_flush_cb
    // work on real hardware, and that colours are not byte-swapped -
    // BEFORE any screen is built on top of it. It deliberately takes over the display
    // and never returns, so it cannot interfere with normal operation.
    //
    // Three things to read off the panel:
    //   1. text appears at all            -> lv_conf + driver + flush are working
    //   2. the bar is RED, not blue       -> LV_COLOR_16_SWAP 0 + setSwapBytes(true)
    //                                        agree; if it is blue they are fighting
    //   3. no tearing/garbage on the band -> the 0x10000/w chunking is right
    {
        Lv::begin(Display::tft);
#if LVGL_SMOKE_LAUNCHER
        // Exercise the ported launcher: build one entry, then slide to another. Verifies
        // the styles, FONT_PCT27, both geometries and the two lv_anims in one go.
        LvLauncher::init();
        LvLauncher::buildStatic(&IMG_APP_BITCOIN, "BitCoin");
        for (;;) {
            LvLauncher::showTransition(&IMG_APP_CLOCK, "Clock", 6, false);
            delay(700);
            LvLauncher::showTransition(&IMG_APP_BITCOIN, "BitCoin", 5, false);
            delay(700);
        }
#endif
        lv_obj_t* scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

        lv_obj_t* lbl = lv_label_create(scr);
        // CANARY: "Clear" is the string that exposed the old generator corruption
        // (it rendered as "Cmfbs" after a backslash-in-comment ate a glyph row), and
        // the degree symbol is what the ASCII-only charset used to drop. Both in the
        // original's own BODY27 face, verbatim from the binary.
        lv_label_set_text(lbl, "Clear 18°C");
        lv_obj_set_style_text_font(lbl, &FONT_BODY27, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

        // Pure red. RGB565 0xF800. If this renders BLUE the byte order is wrong.
        lv_obj_t* bar = lv_obj_create(scr);
        lv_obj_set_size(bar, 160, 24);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, 20);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);

        Serial.printf("[lv] smoke: free=%u internal=%u largest_int=%u\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        for (;;) { Lv::tick(); delay(5); }
    }
#endif
    Serial.println("[boot] display init done");

    // 2. LEDC backlight.
    // CORRECTION: this used to be 0xf0 "from helper(0xf0)". That function is NOT the
    // SPLASH brightness only. The configured value cannot be read yet, because LittleFS is
    // not mounted until step 3 below - so set a fixed level here and load the real one after
    // the mount. The backlight itself is ledcWrite(channel, duty), fed from /.sys/brt.json
    // key "brt", defaulting to BRT_DEFAULT when the file is absent.
    //
    // This block used to try reading brt.json right here, which silently failed on every boot
    // (no filesystem, so readJson always returned false) and left brightness pinned at the
    // default. A saved brightness therefore applied instantly via the web handler but was lost
    // on the next reboot. The real read now lives after the mount, in step 4b.
    Display::setDayBrightness((uint8_t)BRT_DEFAULT);
    Serial.printf("[disp] splash brightness %d\n", BRT_DEFAULT);

    // 3. LittleFS - helper(true, "/littlefs", 10, "spiffs")
    //    partition label "spiffs" is the actual flash label used in partitions.csv
    BLK8("pre-fs");
    if (!LittleFS.begin(true, "/littlefs", 20, "spiffs")) {
        // Nothing can work without the filesystem; bail out rather than half-boot.
        return;
    }

    // A filesystem OTA asked to keep the WiFi settings leaves them in NVS, because the image it
    // wrote took /.sys with it. Claim them BEFORE loadWiFiConfig reads the credentials, and note
    // that the call erases the stash whether or not it was needed.
    WiFiMgr::restoreStashedCredentials();

    // 4. readConfig
    readConfig();

    // 4b. Configured brightness. HAS to be after the mount: brt.json lives on LittleFS.
    //     The file stores the RAW active-low duty as a STRING ({"brt":"195"}), so it needs
    //     readInt -- `| BRT_DEFAULT` discards strings and would reinstate the default.
    {
        DynamicJsonDocument bdoc(128);
        int brt = BRT_DEFAULT;
        if (FSUtil::readJson(PATH_BRT, bdoc)) brt = FSUtil::readInt(bdoc, "brt", BRT_DEFAULT);
        if (brt < 0)   brt = 0;
        if (brt > 255) brt = 255;
        Display::setDayBrightness((uint8_t)brt);   // sets the day level night mode reads
        Serial.printf("[disp] brightness %d\n", brt);
    }

    // 5. Ensure required directories and seed files exist
    //    Firmware creates these on first boot if missing.
    if (!LittleFS.exists("/image")) {
        LittleFS.mkdir("/image");
    }
    // /image/80x80.gif is a DIRECTORY on a real device - it holds the weather GIFs, and the
    // default weather GIF path is "/image/80x80.gif/spaceman.gif".
    //
    // This used to create it by opening it for WRITING, which makes a zero-byte FILE instead.
    // Two things broke as a result: nothing could ever be stored under that path, so uploading
    // a weather GIF had nowhere to go; and the Picture app enumerated the zero-byte file as an
    // image (it is a real file whose name ends in .gif, so the isDirectory() filter did not
    // exclude it), failed to decode it, and fell back to drawing the bare text "80x80.gif" on
    // an otherwise black screen - which is what an empty picture library actually looked like.
    {
        const char* WXGIF_DIR = "/image/80x80.gif";
        File probe = LittleFS.open(WXGIF_DIR);
        const bool wrongType = probe && !probe.isDirectory();
        if (probe) probe.close();
        if (wrongType) {
            LittleFS.remove(WXGIF_DIR);          // drop the zero-byte file left by older builds
            Serial.printf("[fs] replaced stray file %s with a directory\n", WXGIF_DIR);
        }
        if (!LittleFS.exists(WXGIF_DIR)) LittleFS.mkdir(WXGIF_DIR);
    }
    if (!LittleFS.exists(SYS_DIR)) {
        LittleFS.mkdir(SYS_DIR);
    }
    // Seed missing config files.  mklittlefs skips dotfile directories,
    // so /.sys/ files are never in the built image and must be seeded here.
    // The full inventory is 25 files - the same set the factory reset removes. Seeding only 9
    // is why the boot log used to complain about tz.json / ntp.json being absent.
    static const char* seedPaths[] = {
        "/.sys/brt.json",
        "/.sys/ntp.json",
        "/.sys/tz.json",
        "/.sys/city.json",
        "/.sys/key.json",
        "/.sys/stock2.json",
        "/.sys/monitor.json",
        "/.sys/k.json",
        "/.sys/stk.json",
        "/.sys/ccolor.json",
        "/.sys/c_theme.json",
        "/.sys/c_chart.json",
        "/.sys/sep.json",
        "/.sys/bili.json",
        "/.sys/delay.json",
        "/.sys/timebrt.json",
        "/.sys/hour12.json",
        "/.sys/day.json",
        "/.sys/timecolor.json",
        "/.sys/coin2.json",
        "/.sys/unit.json",
        "/.sys/gif.json",
        "/.sys/album.json",
        nullptr
    };
    // IMPORTANT: NO DEFAULT CONFIG FILES SHOULD BE WRITTEN AT ALL - every reader guards on
    // the file existing and falls back to its own default when it does not.
    // Seeding exists only to stop LittleFS.exists() spamming vfs_api errors, so the CONTENT must
    // not invent behaviour: anything without a known default is seeded as `{}` so the reader
    // still falls through to its own.
    static const char* seedJson[] = {
        "{\"brt\":180}",   // brt: default 180
        "{}",              // ntp   - no known default
        "{}",              // tz    - the reader clamps offset to -720..840 min
        "{}",              // city
        "{}",              // key
        "{}",              // stock2  (an invented AAPL/MSFT/GOOG default was removed)
        "{}",              // monitor (an invented m_i=5 default was removed)
        "{}",              // k
        "{}",              // stk
        "{}",              // ccolor  (previously invented up/down colours)
        "{}",              // c_theme
        "{}",              // c_chart
        "{}",              // sep     (previously invented "." / ",")
        "{}",              // bili    - purpose still undetermined
        "{\"delay\":0}",   // delay: 0 = no boot countdown
        "{\"en\":false}",  // timebrt: absent file => en=0 (readNightModeConfig).
                           // The firmware has NO defaults for t1/t2/b2 - do not invent them.
        "{\"h\":0}",       // hour12 "h": default 0 = 24h
        "{\"day\":1}",     // day: default 1, and 0 is coerced to 1
        // Seeding the wrong timecolor values here once OVERRODE AppClock's correct defaults,
        // simply because the file existed.
        "{\"hc\":\"FFFFFF\",\"mc\":\"FF5A00\",\"sc\":\"FF5900\"}",
        "{}",              // coin2  (previously invented BTCUSDT/ETHUSDT/... list)
        "{}",              // unit   (previously invented metric/C/hPa)
        "{}",              // gif    - "0"/non-path means no animated icon
        "{}",              // album
        nullptr
    };
    // *** SEEDING IS DISABLED. ***
    // A config file should be created only when the user SAVES that app's settings; until
    // then every reader falls through to its in-code default. Pre-seeding inverts that - a
    // seeded file always wins over the default - which is exactly how the wrong timecolor
    // values once overrode AppClock. A device in normal use has only a handful of these
    // files and 404s on the rest.
    // The log spam that motivated seeding is handled by the exists() guard in
    // FSUtil::readFile, so nothing is written here any more.
    (void)seedPaths; (void)seedJson;   // retained above purely as the documented inventory
    //
    // app.json is NOT seeded either. This block used to fabricate
    // {"theme":6, "apps":{...9 booleans...}} whenever it was missing, which was wrong three
    // ways: the default startup app is -1 (launcher), not 6 (Clock); theme is a STRING with
    // INT flags, not a number with booleans; and because sortApps reads theme string-only,
    // the seeded 6 was silently read back as -1 anyway. The file existed and did not even
    // have the intended effect.
    //
    // With no file, sortApps enables every app and leaves the startup app as -1, so a factory
    // reset lands in the launcher.

    // 6. Show firmware version then "Booting..."
    displayStatusFor(FW_VERSION, 1000);
    displayStatus("Booting...");

    // 7. Load WiFi credentials + optional boot-delay countdown
    
    int countdown = 0;
    loadWiFiConfig(&countdown);
    delay(200);

    // Countdown display loop - s_Delay_3f444136 "Delay %d"
    while (countdown > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Delay %d", countdown);
        displayStatusFor(buf, 1000);
        countdown--;
    }

    // 8. BOOT-LOOP GUARD, via two marker files:
    //
    //      if      (!exists(/r1.flag))  create /r1.flag
    //      else if (!exists(/r2.flag))  create /r2.flag
    //      else  { remove both; show "STOP AUTO APP..."; delay 500; skip the startup app }
    //
    // The point is that a startup app which crashes during onEnter cannot wedge the device:
    // the third consecutive boot lands in the launcher instead, where the settings UI is
    // reachable. Skipping is done by not calling restoreLastApp() further down, the same way
    // AP mode skips it.
    //
    // This used to be delete-only, and with the names misspelled ".r1_flag"/".r2_flag". That
    // was self-consistent - the same code wrote and removed its own spelling - but it meant
    // the guard did not exist, and a device that already had a real /r1.flag kept it forever.
    //
    // The markers are also CLEARED once the device has been up for BOOT_HEALTHY_MS (see
    // AppCtrl::runLoop). Without that the counter only resets on the STOP boot itself, so
    // every third power-cycle would skip the startup app even when nothing was wrong - an
    // annoyance rather than a protection.
    bool stopAutoApp = false;
    {
        if (!LittleFS.exists(OTA_FLAG_R1)) {
            File f = LittleFS.open(OTA_FLAG_R1, "w");
            if (f) f.close();
        } else if (!LittleFS.exists(OTA_FLAG_R2)) {
            File f = LittleFS.open(OTA_FLAG_R2, "w");
            if (f) f.close();
        } else {
            LittleFS.remove(OTA_FLAG_R1);
            LittleFS.remove(OTA_FLAG_R2);
            LOGF("[boot] boot-loop guard tripped: skipping the startup app");
            displayStatusFor("STOP AUTO APP...", 500);
            stopAutoApp = true;
        }
    }

    // 9. WiFi connect + AP fallback loop
    //    loadWiFiConfig() called WiFi.begin(ssid,pass) if credentials exist in files,
    //    or skipped WiFi.begin() entirely if no credentials -> device will enter AP mode.
    //    Disconnect with NVS erase so the SDK never auto-reconnects to stale credentials.
    // g_wifiSsid is the FILE-sourced SSID. Never use WiFi.SSID() for decisions - it
    // returns the SDK/NVS-cached value, which is why a factory reset appeared not to
    // work: the files were gone but the stale NVS SSID still showed up and the SDK
    // auto-reconnected during our wait loop.
    Serial.printf("[wifi] file ssid='%s'\n", g_wifiSsid.c_str());
    bool apMode = false;

    if (g_wifiSsid.isEmpty()) {
        // EMPTY-SSID SHORT-CIRCUIT: show the prompt and go straight to SoftAP. WiFi.begin()
        // is never called, so the SDK cannot auto-connect from a stale NVS record.
        // Note the prompt ends with '!', not an ellipsis.
#ifndef BOARD_SPOTPEAR_MUMA_S3
        displayStatusFor("Please connect WiFi " AP_SSID "!", 1000);
#endif
        apMode = true;
    } else {
        // Try connecting, refreshing "Try <ssid>" every 600 ms.
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Try %s", g_wifiSsid.c_str());
            displayStatusFor(buf, 600);
        }
    }

    if (apMode || WiFi.status() != WL_CONNECTED) {
        // AP fallback path. "Connect failed..." is only shown when an attempt actually
        // failed, not on the empty-SSID path.
        if (!apMode) {
            displayStatusFor("Connect failed...", 1000);
        }

        WiFi.disconnect(true);
        delay(180);
        WiFi.softAP(AP_SSID);

        setupCaptivePortal();

        // DNSServer requires processNextRequest() polling - run it on a background task
        // so the main loop (AppCtrl::runLoop) doesn't need to know about it.
        xTaskCreate([](void*) {
            for (;;) {
                captivePortalHandle();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }, "dns", 2048, nullptr, 1, nullptr);

        // The web server runs in the AP path too - it is how the user configures WiFi.
        installAppControlHooks();
        BLK8("pre-web");
        // ASYNC :8080 SERVER REMOVED. It existed only as an OTA safety net while the
        // synchronous server was unproven. /update and /doUpload now both live on :80, and
        // an OTA over :80 has been flashed and verified end to end. Removing it drops the async_tcp task's 16 KB
        // stack and ESPAsyncWebServer's per-ack malloc pattern - the exact fragmentation
        // source documented at the top of SyncWeb.cpp. A serial upload remains the backstop.
        // See docs research note: esp32-web-server-libraries.md recommends the IDF httpd
        // outright, and ESPAsyncWebServer's crash-under-load record is why it is gone.
        SyncWeb::begin();      // synchronous esp_http_server -> :80 (serves the whole UI)
        BLK8("post-web");

        Serial.printf("[heap] AP pre-sprite: free=%u max=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

        // s_Start_AP_mode____3f4441c9 / s_http___192_168_4_1_3f4441da
#ifndef BOARD_SPOTPEAR_MUMA_S3
        displayStatusFor("Start AP mode...", 2000);
        displayStatusFor(AP_URL, 5000);
#endif

        apMode = true;

    } else {
        // Connected path - s_Connect_Success____3f444188
        // Disable STA modem power-save so the device answers ARP/TCP promptly -
        // power-save was why PC->device requests timed out.
        WiFi.setSleep(false);
        displayStatusFor("Connect Success!", 500);

        // Print full WiFi diagnostics so we can verify the connection
        {
            Serial.printf("[net] SSID:    %s\n",   WiFi.SSID().c_str());
            Serial.printf("[net] BSSID:   %s\n",   WiFi.BSSIDstr().c_str());
            Serial.printf("[net] IP:      %s\n",   WiFi.localIP().toString().c_str());
            Serial.printf("[net] Gateway: %s\n",   WiFi.gatewayIP().toString().c_str());
            Serial.printf("[net] RSSI:    %d dBm\n", WiFi.RSSI());

            char buf[32];
            snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
            displayStatus(buf);
        }

        delay(5000);

        // Read all LittleFS config BEFORE starting the web server.
        // The web server opens files for static serving; reading config first
        // ensures FDs are available and reads are deterministic.

        // NTP config - read paths from LittleFS before web server starts
        // tz.json is JSON, NOT a POSIX TZ string. This used to hand the raw file contents
        // -- literally {"mode":1,"offset":120} -- to configTzTime(), which cannot parse it
        // and silently leaves the clock on UTC. Parse it properly: "offset" is in MINUTES
        // (clamps to -720..840) and "mode" == 1 enables it.
        // Mode-aware: manual uses tz.json "offset", Auto (Weather city) uses the weather
        // city's timezone from city.json "t"/"mt" (Config_getTimezone).
        long tzOffsetMin = NTPTime::resolveOffsetMin();
        String ntpStr = FSUtil::readFile(PATH_NTP);
        if (ntpStr.isEmpty()) ntpStr = NTP_DEFAULT;
        configTime(tzOffsetMin * 60L, 0, ntpStr.c_str(), NTP_ALT1, NTP_ALT2);
        NTPTime::seedOffset(tzOffsetMin);   // so a later web tz save re-applies only on change
        Serial.printf("[time] tz offset %ld min, ntp %s\n", tzOffsetMin, ntpStr.c_str());

        // Night-mode config - read before web server. The per-tick dimming itself lives in
        // AppCtrl::runLoop (_loadNightMode/_applyNightMode, mirroring NightMode_applyBrightness
        // which arduino_loop runs every tick); here we only surface the boot banner.
        {
            bool nightEnabled = false;
            int startH = 22, endH = 7, dimBright = 10;
            readNightModeConfig(&nightEnabled, &startH, &endH, &dimBright);
            if (nightEnabled) {
                displayStatusFor("Night mode is enabled", 2000);
            }
        }

        // Register apps - before web server starts
        g_ctrl.init();
        g_ctrl.addApp(&appSlots);
        g_ctrl.addApp(&appSettings);
        g_ctrl.sortApps();    // reads /.sys/app.json
        // Device loads directly into the launcher menu showing Slots and Settings

        // setupWebServer - helper (started AFTER config reads)
        installAppControlHooks();
        BLK8("pre-web");
        SyncWeb::begin();      // synchronous esp_http_server -> :80 (serves the whole UI)
        BLK8("post-web");

        // mDNS - start before sprite so UDP socket can allocate.
        // Moved after softAP setup below would crash (mode change invalidates socket).
        // If UDP buffer allocation fails it logs a warning; non-fatal.
        MDNS.begin(MDNS_HOST);

        Serial.printf("[heap] connected pre-sprite: free=%u max=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

        // NO simultaneous softAP in connected mode: after the 61KB sprite there is
        // not enough heap to start the AP interface - ieee80211_hostap_attach hits
        // ESP_ERROR_CHECK(no-mem) and aborts.  AP mode remains available as the
        // fallback when no WiFi credentials exist / connection fails.

        // NTP sync wait - firmware polls for up to 20 s
        displayStatusFor("Sync time...", 300);
        {
            unsigned long t0    = millis();
            unsigned long tNTP  = millis();
            unsigned long tDisp = 0;
            while (time(nullptr) < 1700000000UL && millis() - t0 < 20000) {
                unsigned long now = millis();
                if (tDisp == 0 || now - tDisp > 3000) {
                    char buf[48];
                    snprintf(buf, sizeof(buf), "Sync time: %lus",
                             (unsigned long)(20000 - (now - t0)) / 1000);
                    displayStatus(buf);
                    tDisp = now;
                }
                if (now - tNTP > 12000) {
                    configTime(tzOffsetMin * 60L, 0, ntpStr.c_str(), NTP_ALT1, NTP_ALT2);
                    tNTP = now;
                }
                delay(100);
            }
        }
    }

    // 10. "Don't Touch Top" warning before touch is initialised
    //     s_Don_t_Touch_Top__3f444211
#ifndef BOARD_SPOTPEAR_MUMA_S3
    displayStatusFor("Don't Touch Top", 1000);
#endif

    // 11. Touch & Audio init
    BLK8("pre-touch");
    Touch::begin();
    Sound::begin();
    BLK8("post-touch");

    // Additional init - helper (unknown; possibly LEDC fine-tune or timer)

    if (apMode) {
        // AP path: register apps after web server is already running.
        // No WiFi, so NTP/config reads use defaults gracefully.
        g_ctrl.init();
        g_ctrl.addApp(&appSlots);
        g_ctrl.addApp(&appSettings);
        g_ctrl.sortApps();
    }

    // 12. Start scheduler - AppCtrl_runLoop, never returns.
    //     Internally calls Scheduler_run on each tick.
    g_ctrl.runLoop();

    // Unreachable: runLoop() never returns.
}

// loop() is not used - the firmware's main loop runs inside AppCtrl::runLoop()
// which is called from setup() and never returns.  Arduino's loopTask will call
// loop() in a spin after setup() exits, but the firmware never exits setup().
void loop() {
    delay(1000);
}
