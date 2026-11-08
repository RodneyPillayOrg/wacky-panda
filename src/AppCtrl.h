/*
 * AppCtrl - the application controller.
 *
 * Owns the launcher, the two-mode (launcher / active app) state machine, the app-enable
 * flags from /.sys/app.json, touch dispatch, night mode, and the single lv_timer_handler
 * pump the whole firmware runs off. runLoop() never returns.
 *
 * Transition animation:
 *   +/-184 px horizontal slide, linear, 460 ms (speed 400 via lv_anim_speed_to_time).
 *   Icon format: LVGL LV_IMG_CF_INDEXED_4BIT 128x128 (see AppIcons.h).
 *   Forward  (dir 6, SINGLE tap, scroll +1): new icon slides in from right (+184 -> 0).
 *   Backward (dir 5, double tap, scroll -1): new icon slides in from left (-184 -> 0).
 *   Gesture mapping:
 *   click count 1 fires slot +0x88 = touch_onClick_scroll = scroll(+1);
 *   count 2 fires +0xb8 = touch_onDoubleClick_scroll = scroll(-1).
 */
#pragma once
#include <Arduino.h>
#include <vector>
#include <time.h>
#include "Config.h"
#include "TimeNow.h"
#include "apps/AppBase.h"
#include "Display.h"
#include "NTPTime.h"
#include "LvglDisplay.h"
#include "LvLauncher.h"
#include <esp_heap_caps.h>
#include "LvImagesGen.h"
#include "Touch.h"

class AppCtrl {
public:
    void init() {
        _apps.clear();
        _current = 0;
        _dwellMs = 30000;
        _reloadConfig();
    }

    void addApp(AppBase* app) {
        if (app) _apps.push_back(app);
    }

    // In an APP this pages the app (as a tap does). In the LAUNCHER it scrolls to the next /
    // previous icon (as a tap does there). It used to return early in the launcher, so the
    // web UI's arrows appeared dead until an app happened to be running.
    //
    // The web UI sends /set?page for its next/prev arrows; making them work in BOTH the
    // launcher and a running app is what this hook is for.
    //
    // Deferred to the main task: the launcher path runs a transition animation and the app
    // path calls into app code, neither of which is safe from the web task.
    void pageStep(int dir) {
        if (_apps.empty()) return;
        _pendingPageStep += (dir >= 0) ? 1 : -1;
    }

    // Web-config live-apply signal. A "Save ..." from the web must reach the RUNNING app
    // immediately - e.g. the Clock's 1 Hz timer reads the shared colour buffers every tick,
    // and a web save reloads those buffers.
    // /set sets this flag (safe from the web task - it only writes a bool),
    // and runLoop consumes it on the MAIN task by re-running the active app's reloadConfig()
    // (calling reloadConfig directly from the web task would race vectors/LVGL state). Any
    // config the app's render/timer reads live (clock colours, 12/24h, day fmt, coin prices)
    // then updates without a re-entry.
    void requestReload() { _pendingReload = true; }

    // Web equivalent of a long press (/set?enter): enter the app the launcher is showing, or
    // exit the running app back to the launcher. Deferred rather than acted on here, for the
    // same reason as requestReload: entering an app runs onEnter(), which reallocates the
    // draw band and touches LVGL state, and doing that from the web task races the renderer.
    void requestEnterToggle() { _pendingEnterToggle = true; }

    // Reads /.sys/app.json.
    // SCHEMA:
    //   { "theme": <int -1..8>,            // last/startup app INDEX
    //     "apps": { "bitcoin":bool, "app_coingecko":bool, "app_stock":bool,
    //               "app_weather":bool, "app_picture":bool, "app_monitor":bool,
    //               "app_clock":bool, "app_idea":bool, "app_settings":bool } }
    // There is NO "rot_i" and NO "delay" key; both are orphans from older revisions. Note
    // the first flag key is bare "bitcoin", not "app_bitcoin". Flags are in registration
    // order, and if every flag is false slot 0 is force-enabled.
    void sortApps() {
        DynamicJsonDocument doc(512);
        bool ok = FSUtil::readJson(PATH_APP, doc);
        Serial.printf("[ctrl] sortApps: readJson=%d apps=%d\n", (int)ok, (int)_apps.size());

        _applyEnableFlags(doc, ok);
        // Count unconditionally - previously this only counted inside the block above, so
        // an app.json without an "apps" key logged a misleading "enabled=0".
        int enabledCount = 0;
        for (int i = 0; i < (int)_apps.size() && i < 2; i++) if (_enabled[i]) enabledCount++;
        if (enabledCount == 0) { _enabled[0] = true; enabledCount = 1; }  // orig force-enables slot 0

        // "theme" = startup app index, validated -1..8. On a real device it is a STRING
        // ("-1" when no last app has been recorded), so parse it as text.
        int theme = -1;
        // Read via readInt, which coerces EITHER a string or a number. This used to be
        // as<const char*>() with a "-1" fallback, i.e. string-only - and as<const char*>()
        // returns nullptr for a JSON number. So any app.json holding a numeric theme silently
        // read as -1. That is not hypothetical: _saveCurrent() below wrote a number, so
        // entering an app recorded a startup app that the next boot could not read, and the
        // whole feature only appeared to work when the web UI (which sends a string) had been
        // used. Files in the wild now contain both forms, so accept both.
        if (ok) theme = FSUtil::readInt(doc, "theme", -1);
        // theme is BOTH the launcher's starting position and the "startup app" to enter, but
        // -1 has to survive: it means "no app was recorded, stay in the launcher". Clamping
        // -1 to 0 here (as this used to) destroyed that distinction, which is why the startup
        // app never worked. Valid means in range AND enabled; otherwise none.
        _startupApp = (theme >= 0 && theme < (int)_apps.size() && _enabled[theme]) ? theme : -1;
        _current = (_startupApp >= 0) ? _startupApp : _firstEnabled();
        Serial.printf("[ctrl] sortApps: theme=%d startup=%s enabled=%d shown=%s\n",
                      theme,
                      _startupApp >= 0 ? _apps[_startupApp]->name() : "(none)",
                      enabledCount, _apps[_current]->name());
    }

    // Restore the startup app - the WiFi-connected boot path only.
    //
    // This does NOT merely select it in the launcher: when the saved index is in range and
    // enabled it switches to active mode and calls that app's onEnter, so the device boots
    // straight into the app. It used to just re-run sortApps(), which
    // only moves the launcher's cursor - so a device with "startup app = Clock" sat on the
    // Clock ICON instead of running the Clock. Nothing called this at all, either.
    //
    // The actual entering is deferred to runLoop rather than done here, because onEnter draws
    // and the display/LVGL is only brought up on the first launcher draw. Skipped in AP mode.
    void restoreLastApp() { _startupPending = true; }

    // Name of the currently selected app (launcher entry or running app).
    const char* currentName() const {
        if (_apps.empty() || _current < 0 || _current >= (int)_apps.size()) return "";
        return _apps[_current]->name();
    }

    // Web/API entry point. Two defects this had, both user-visible:
    //
    // 1) NAME MATCHING. The web UI sends the LAUNCHER LABEL, not the internal id:
    //    `data-open-app="CoinGecko" data-app-key="coingecko"`. Comparing only internal ids
    //    ("app_coingecko") made every web "Open This App" answer "no such app". Match the
    //    LABEL first, then the id.
    //
    // 2) IT MUST NOT ENTER THE APP HERE - only record a REQUEST, which runLoop consumes on
    //    the MAIN task. Calling onEnter() straight from the web task raced the render loop on
    //    the other core, which is why an HTTP activation appeared to do nothing: the main
    //    loop immediately re-entered the launcher.
    bool activateByName(const String& name) {
        for (int i = 0; i < (int)_apps.size(); i++) {
            const char* id = _apps[i]->name();
            if (name == _displayName(id) || name == id) {
                if (!_enabled[i]) return false;   // a disabled app cannot be opened
                _pendingEnter = i;                // consumed by runLoop (main task)
                return true;
            }
        }
        return false;
    }

    void runLoop() {
        if (_apps.empty()) return;

        if (_current < 0 || _current >= (int)_apps.size()) _current = 0;

        // Draw the launcher first: that is what brings LVGL up (lazily), so it must happen
        // before any app's onEnter can draw.
        _mode = Mode::Launcher;
        _drawLauncher();
        _dwellStart = millis();

        // Then, if a startup app is configured AND this is the WiFi path, enter it: boot
        // straight into the saved app rather than sitting on its icon. With no saved app (-1)
        // the launcher simply stays put.
        if (_startupPending && _startupApp >= 0 && _startupApp < (int)_apps.size()) {
            _startupPending = false;
            _current = _startupApp;
            Serial.printf("[ctrl] startup app -> %s\n", _apps[_current]->name());
            _enterCurrent();
        }
        _startupPending = false;

        bool nmEn = false; int nmStart = 22, nmEnd = 7, nmBright = 10;
        unsigned long lastNMCheck = 0;
        unsigned long lastHeapLog = 0;
        // Boot-loop guard reset. See the guard itself in main.cpp step 8: it counts
        // consecutive boots with two marker files and drops to the launcher on the third.
        // Clearing them once the device has genuinely run for a while is what turns that
        // counter into "consecutive FAILED boots" rather than "every third power-cycle".
        //
        // Clearing on a healthy run is what makes this a guard rather than a nuisance --
        // boot sequence touches either string -- so its counter only resets on the STOP boot.
        bool bootHealthy = false;
        const unsigned long bootStart = millis();

        for (;;) {
            if (!bootHealthy && millis() - bootStart >= BOOT_HEALTHY_MS) {
                bootHealthy = true;
                FSUtil::Guard fsg;
                if (LittleFS.exists(OTA_FLAG_R1)) LittleFS.remove(OTA_FLAG_R1);
                if (LittleFS.exists(OTA_FLAG_R2)) LittleFS.remove(OTA_FLAG_R2);
                LOGF("[ctrl] boot healthy after %lums, boot-loop markers cleared",
                     (unsigned long)BOOT_HEALTHY_MS);
            }
            // Heap telemetry - find leaks by watching free/max per active app
            if (millis() - lastHeapLog >= 10000UL) {
                // blk8 is the number that decides whether a TLS handshake can happen at
                // all: mbedTLS wants a large CONTIGUOUS MALLOC_CAP_8BIT block, and
                // getMaxAllocHeap() reports MALLOC_CAP_DEFAULT, which can be far larger.
                // Logged here so the trend across launcher/app is visible without a probe.
                LOGF("[heap] free=%u blk8=%u blkdef=%u app=%s mode=%s touch=%u",
                     ESP.getFreeHeap(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     ESP.getMaxAllocHeap(),
                     _apps[_current]->name(),
                     _mode == Mode::Launcher ? "launcher" : "active",
                     Touch::raw());
                lastHeapLog = millis();
            }
            // Night mode check every 60 s
            unsigned long now = millis();
            if (now - lastNMCheck >= 60000UL || lastNMCheck == 0) {
                _loadNightMode(nmEn, nmStart, nmEnd, nmBright);
                lastNMCheck = now;
            }
            _applyNightMode(nmEn, nmStart, nmEnd, nmBright);

            // Consume a pending web activation ON THIS TASK - activateByName only records
            // the request.
            // Live-apply a web config save to the running app.
            // Consumed here on the MAIN task so reloadConfig never races the web task.
            if (_pendingReload) {
                _pendingReload = false;
                // A web tz save must re-apply the offset live (configTime only ran at boot).
                // tz is global, so do it regardless of which app is active. Cheap + guarded to
                // only restart SNTP when the offset actually changed.
                if (NTPTime::applyTimezoneLive())
                    Serial.printf("[ctrl] tz re-applied\n");
                // Force the night-mode config to be re-read on the NEXT iteration. It is
                // otherwise on its own 60 s timer, which a save did not reset - so toggling
                // night mode, or changing its window or brightness, took up to a minute to
                // do anything and read as "night brightness does not apply". Every other
                // web save takes effect within a tick; this one now does too.
                lastNMCheck = 0;
                // A save may have toggled an app on or off; the rotation must follow.
                _refreshEnableFlags();
                if (_mode == Mode::Active && !_apps.empty()) {
                    _apps[_current]->reloadConfig();
                    Serial.printf("[ctrl] live-reload %s\n", _apps[_current]->name());
                }
            }

            // Web arrow equivalent, taking exactly the path a tap takes in each mode.
            while (_pendingPageStep != 0) {
                const int dir = (_pendingPageStep > 0) ? 1 : -1;
                _pendingPageStep -= dir;
                if (_mode == Mode::Launcher) {
                    _scroll(dir);
                } else {
                    _tapCounter += dir;
                    _apps[_current]->onTapCounter(_tapCounter);
                }
            }

            // Web long-press equivalent, taking exactly the path TouchEvent::Long takes.
            if (_pendingEnterToggle) {
                _pendingEnterToggle = false;
                if (_apps.empty()) {
                    Serial.printf("[ctrl] web enter ignored: no apps\n");
                } else if (_mode == Mode::Launcher) {
                    _enterCurrent();
                } else {
                    _exitToLauncher();
                    continue;   // the app is gone; restart the loop rather than tick it
                }
            }

            if (_pendingEnter >= 0) {
                int target = _pendingEnter;
                _pendingEnter = -1;
                if (target < (int)_apps.size()) {
                    if (_mode == Mode::Active) {
                        if (_current == target) continue;   // already running it
                        _apps[_current]->onExit();
                        _mode = Mode::Launcher;
                    }
                    _current = target;
                    _enterCurrent();
                }
                continue;
            }

            // -- Pump LVGL, unconditionally, once per iteration --------------------
            // This is the firmware's ONLY lv_timer_handler caller, and it runs on every
            // iteration in every mode.
            //
            // This is what was missing. An app's onEnter would lv_scr_load its screen and
            // then nothing ever redrew, so the panel kept showing the last thing the
            // launcher had flushed - the Clock built correctly and was simply never drawn.
            // EXCEPTION: the two GuiLite-style apps (Idea, Picture) draw full-screen via the
            // banded TFT path (Display::renderApp), not LVGL. While one of them is active we
            // must NOT pump lv_timer_handler - otherwise LVGL's flush of the stale launcher
            // screen and the app's DMA band-pushes fight over the SPI bus and tear/flicker.
            // For every LVGL app, and for the launcher, pump normally.
            const bool directDraw =
                (_mode == Mode::Active && _apps[_current]->usesDirectDraw());
            if (!directDraw) Lv::tick();

            if (_mode == Mode::Active) {
                // Run the active app's loop. A FALSE return means "skip me": run onExit and
                // transition back to the launcher.
                if (!_apps[_current]->loop()) { _exitToLauncher(); continue; }
                // NO AUTO-ROTATION: no timer, no elapsed comparison, no rotation advance.
                // The index only ever changes from a touch or from the web open_app. An
                // earlier dwell-based rotation here was invented; /.sys/delay.json is the
                // pre-WiFi BOOT countdown ("Delay %ds") and has nothing to do with apps.
            }
            // Launcher mode is static: the icon+label stays until the user acts,
            // matching the captured first-run/post-config behaviour.

            // Touch dispatch - original semantics (initTouch):
            //   launcher: tap scrolls the list, long-press (>=500 ms) enters the app
            //   active:   tap is forwarded to the app, long-press leaves the app
            TouchEvent ev = Touch::poll();
            if (ev != TouchEvent::None) {
                Serial.printf("[touch] EVENT %s (mode=%s)\n",
                              ev == TouchEvent::Short  ? "SHORT"  :
                              ev == TouchEvent::Double ? "DOUBLE" : "LONG",
                              _mode == Mode::Launcher ? "launcher" : "active");
            }
            switch (ev) {
                case TouchEvent::Short:
                    // SINGLE tap = scroll(+1) / counter++. PROVEN from the gesture library's
                    // SINGLE tap: scroll FORWARD (+1) in the launcher, or step the active
                    // app's page forward. These signs were briefly inverted once - single is
                    // forward, double is back.
                    if (_mode == Mode::Launcher) _scroll(1);
                    else { _tapCounter++; _apps[_current]->onTapCounter(_tapCounter); }
                    break;
                case TouchEvent::Double:
                    // DOUBLE tap: scroll BACKWARD (-1).
                    if (_mode == Mode::Launcher) _scroll(-1);
                    else { _tapCounter--; _apps[_current]->onTapCounter(_tapCounter); }
                    break;
                case TouchEvent::Long:
                    if (_mode == Mode::Launcher) _enterCurrent();
                    else                         _exitToLauncher();
                    break;
                default:
                    break;
            }

            yield();
        }
    }

private:
    // Two-mode state machine: the launcher / icon menu, or one active app.
    enum class Mode { Launcher, Active };
    Mode _mode = Mode::Launcher;
    bool _lvReady = false;      // LVGL brought up lazily, on first launcher draw

    std::vector<AppBase*> _apps;
    // Per-app enable flags from app.json "apps". A disabled app is hidden from the launcher.
    bool         _enabled[2] = { true, true };
    int          _current   = 0;
    int          _tapCounter = 0;   // the shared monotonic tap counter apps latch and diff
    // Deferred web activation, written by activateByName on the web task and consumed by
    // runLoop on the main task.
    volatile int _pendingEnter = -1;
    // Web "Save": reload the running app's config on the MAIN task (see requestReload()).
    volatile bool _pendingReload = false;
    volatile bool _pendingEnterToggle = false;   // set by /set?enter on the web task
    int  _startupApp     = -1;      // app.json "theme": app to ENTER at boot, -1 = none
    bool _startupPending = false;   // set by restoreLastApp() on the WiFi boot path
    volatile int _pendingPageStep = 0;   // set by /set?page on the web task
    unsigned long _dwellMs  = 30000;
    unsigned long _dwellStart = 0;
    String        _startApp;

    void _reloadConfig() {
        DynamicJsonDocument doc(256);
        if (FSUtil::readJson(PATH_DELAY, doc)) {
            // readInt: delay.json holds a STRING ("30"), which `| 30` would discard. Same key
            // and same trap as the boot countdown in BootConfig.h. No clamp here -- this is a
            // dwell between app switches, so a large value is merely slow, not unrecoverable.
            int secs = FSUtil::readInt(doc, "delay", 30);
            if (secs < 1) secs = 1;
            _dwellMs = (unsigned long)secs * 1000UL;
        }
    }


    // -- Launcher (mode 0) -----------------------------------------------------
    // Draw the current entry's icon + label.
    void _drawLauncher() {
        if (_apps.empty()) return;
        _lvInit();
        // Geometry, styles and the label font all live in LvLauncher.
        LvLauncher::buildStatic(_lvIcon(_current),
                                _displayName(_apps[_current]->name()));
        LvLauncher::pump();
    }

    // Bring LVGL up lazily, on the first launcher draw. runLoop then pumps it every
    // iteration regardless of mode, as arduino_loop does.
    void _lvInit() {
        if (_lvReady) return;
        // Hand back the OLD renderer's 38,400-byte heap sprite before LVGL starts. It was
        // allocated for the boot status screens and freeBand() had never been called
        // anywhere, so it sat there for the whole run. With LVGL's static 38 KB buffer and
        // its pool also resident, keeping it was enough to exhaust DRAM: widget allocation
        // then fails, and LV_USE_ASSERT_MALLOC turns that into a hang rather than a
        // diagnosable error.
        Display::freeBand();
        Lv::begin(Display::tft);
        LvLauncher::init();
        _lvReady = true;
    }

    static const void* _lvIcon(int idx) {
        // LV_APP_ICONS is emitted in APP REGISTRATION ORDER - the same order the apps are
        // added in main.cpp - so it indexes with _current directly.
        if (idx < 0 || idx >= LV_APP_ICON_COUNT) return nullptr;
        return LV_APP_ICONS[idx];
    }

    // Scroll the launcher selection with the 400 ms / +-184 px slide.
    void _scroll(int dir) {
        if (_apps.empty()) return;
        int prev = _current;
        // Skip apps disabled in app.json "apps": they are not in the launcher at all.
        int n = (int)_apps.size();
        for (int step = 0; step < n; step++) {
            _current = (_current + dir + n) % n;
            if (_enabled[_current]) break;
        }
        if (_current == prev) return;      // nothing else enabled
        // showTransition leaves the new icon+label on screen; nothing redraws afterwards.
        _showTransition(prev, _current, dir > 0);
        Serial.printf("[ctrl] launcher -> %s\n", _apps[_current]->name());
    }

    // Long-press in the launcher: enter the shown app.
    void _enterCurrent() {
        _mode = Mode::Active;
        // Reset the shared tap counter on entry so each app's latch-and-diff starts from a
        // known baseline (0). Without this the monotonic counter carried over from earlier
        // apps makes the first tap in a freshly-entered app jump to an arbitrary page.
        _tapCounter = 0;
        // GuiLite-style apps (Idea, Picture) draw full-screen via the banded TFT path, not
        // LVGL. Allocate the 240x80 band for them (freed for LVGL apps). Fails safe: if the
        // band can't be had, renderApp() is a no-op and the app is simply blank, never a
        // crash. LVGL pumping is skipped for these apps in the loop below.
        Display::enableAppBand(_apps[_current]->usesDirectDraw());
        _apps[_current]->onEnter();
        _dwellStart = millis();
        _saveCurrent();
        Serial.printf("[ctrl] enter app %s free=%u max=%u\n",
                      _apps[_current]->name(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }

    // Long-press inside an app: release its resources and return to the launcher.
    void _exitToLauncher() {
        _apps[_current]->onExit();
        Display::enableAppBand(false);   // free the direct-draw band; LVGL owns the panel again
        _mode = Mode::Launcher;
        _drawLauncher();
        Serial.printf("[ctrl] exit to launcher free=%u\n", ESP.getFreeHeap());
    }


    // forward=true -> new icon slides in from the right; forward=false -> from the left.
    //
    // *** THE 400 IS A SPEED, NOT A DURATION. ***  It is the speed argument to
    //     lv_anim_speed_to_time(speed, start, end) = |start - end| * 1000 / speed
    // so with a 184 px travel the animation lasts 184 * 1000 / 400 = **460 ms**, not 400 ms.
    // Reading the speed
    // literal as if it were the duration.
    void _showTransition(int fromIdx, int toIdx, bool forward) {
        (void)fromIdx;
        _lvInit();
        // The +/-184 px slide, lv_anim_path_linear, the 460 ms derived from speed 400, and
        // the label NOT animating are all in LvLauncher::showTransition.
        LvLauncher::showTransition(_lvIcon(toIdx),
                                   _displayName(_apps[toIdx]->name()),
                                   forward ? 6 : LvLauncher::DIR_REVERSE, false);
    }

    // Map internal app name -> human-readable display name (matches firmware strings).
    static const char* _displayName(const char* id) {
        static const struct { const char* id; const char* label; } tbl[] = {
            { "app_slots",     "WackyPanda" },
            { "app_settings",  "Settings"   },
        };
        for (auto& e : tbl) if (strcmp(id, e.id) == 0) return e.label;
        return id;
    }

    static void _loadNightMode(bool& en, int& start, int& end, int& bright) {
        DynamicJsonDocument doc(128);
        en = false; start = 22; end = 7; bright = 10;
        if (FSUtil::readJson(PATH_TIMEBRT, doc)) {
            // timebrt.json stores EVERY value as a STRING ("en":"1","t1":"0","b2":"250"...).
            // ArduinoJson's `doc[k] | def` returns def for a string (is<int>() is false), so the
            // old code silently defaulted the whole window to [22,7) and en to false - night
            // mode never engaged. FSUtil::readInt does the string-or-int coercion.
            en    = FSUtil::readInt(doc, "en", 0) != 0;
            start = FSUtil::readInt(doc, "t1", 22);
            end   = FSUtil::readInt(doc, "t2", 7);
            bright= FSUtil::readInt(doc, "b2", 10);
        }
    }

    // NightMode_applyBrightness. Called EVERY tick (like arduino_loop). Hour
    // granularity only (minutes never consulted); wrap handled as (h>=t1)||(h<t2) when t2<t1;
    // brightness re-asserted UNCONDITIONALLY, including the day level when disabled - so
    // turning night mode off mid-dim restores day immediately. Day brightness is the cached
    // Display::_dayBright, NOT a per-tick brt.json read: the web brightness save updates it
    // via setDayBrightness, so live changes still apply.
    // The `b1` key in timebrt.json is never read.
    static void _applyNightMode(bool en, int nmStart, int nmEnd, int nmBright) {
        const uint8_t dayBright = Display::dayBrightness();
        if (!en) { Display::setBrightness(dayBright); return; }
        struct tm t;
        // Non-blocking: getLocalTime() otherwise waits 5 s for a valid time, which locks
        // the loop up in AP mode where NTP never syncs. No valid clock => stay at day.
        if (!timeNowIfSet(t)) { Display::setBrightness(dayBright); return; }
        int h = t.tm_hour;
        bool isNight = (nmEnd < nmStart)
            ? (h >= nmStart || h < nmEnd)
            : (nmStart <= h && h < nmEnd);
        const uint8_t b = isNight ? (uint8_t)nmBright : dayBright;
        static int s_lastLog = -1;
        if ((int)b != s_lastLog) {
            LOGF("[nm] en=%d h=%d win=[%d,%d) night=%d day=%u b2=%d -> brt=%u",
                 (int)en, h, nmStart, nmEnd, (int)isNight, dayBright, nmBright, b);
            s_lastLog = (int)b;
        }
        Display::setBrightness(b);
    }

    // Persist the current app index. Original key is "theme" (an INDEX), and the file
    // also carries the per-app enable flags, which we preserve rather than clobber.
    void _saveCurrent() {
        if (_apps.empty()) return;
        DynamicJsonDocument doc(512);
        FSUtil::readJson(PATH_APP, doc);      // keep existing "apps" flags
        appJsonDropOrphanKeys(doc);
        // As a STRING, which is what the web UI sends and what sortApps reads back. The
        // reader accepts both now, but writing one consistent form keeps the file uniform.
        doc["theme"] = String(_current);
        FSUtil::writeJson(PATH_APP, doc);
    }

    // Per-app enable flags, in registration order.
    void _applyEnableFlags(JsonDocument& doc, bool ok) {
        static const char* const PREFIXED[2] = {
            "app_slots", "app_settings"
        };
        static const char* const BARE[2] = {
            "slots", "settings"
        };
        for (int i = 0; i < 2 && i < (int)_apps.size(); i++) _enabled[i] = true;
        if (ok && doc.containsKey("apps")) {
            JsonObject apps = doc["apps"];
            for (int i = 0; i < 2 && i < (int)_apps.size(); i++) {
                const char* k = apps.containsKey(PREFIXED[i]) ? PREFIXED[i] : BARE[i];
                _enabled[i] = (apps[k] | 1) != 0;
            }
        }
    }

    // Re-read ONLY the enable flags, for a web save that toggled an app on or off.
    void _refreshEnableFlags() {
        DynamicJsonDocument doc(512);
        const bool ok = FSUtil::readJson(PATH_APP, doc);
        _applyEnableFlags(doc, ok);

        int enabledCount = 0;
        for (int i = 0; i < (int)_apps.size() && i < 2; i++) if (_enabled[i]) enabledCount++;
        if (enabledCount == 0) { _enabled[0] = true; }   // never leave the list empty

        // If the app we are sitting on just got disabled, move off it: leave a running app,
        // or advance the launcher to the next one that is still enabled.
        if (_current >= 0 && _current < (int)_apps.size() && !_enabled[_current]) {
            if (_mode == Mode::Active) {
                Serial.printf("[ctrl] active app %s disabled -> launcher\n",
                              _apps[_current]->name());
                _exitToLauncher();
            }
            const int next = _firstEnabled();
            if (next != _current) {
                const int prev = _current;
                _current = next;
                _showTransition(prev, _current, true);
            }
        }
    }

    int _firstEnabled() const {
        for (int i = 0; i < (int)_apps.size(); i++) if (_enabled[i]) return i;
        return 0;
    }
};
