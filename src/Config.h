#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>     // FSUtil's LittleFS mutex (see the note by FSUtil::Guard)

// -- Hardware - verified against ESPHome SmallTV Pro config --------------------
// SPI display (ST7789V 240x240):
//   SCLK=18, MOSI=23, DC=2, RST=4, CS=N/C (tied to GND on PCB)
//   Backlight: GPIO25, inverted PWM (LOW = bright)
// Source: esphome yaml (authoritative); giovi321 pins are for NM-TV-154 (different board).
#define PIN_TFT_BL    25
#define BL_PWM_CHAN   0
#define BL_PWM_FREQ   5000
#define BL_PWM_RES    8

// Touch button - GPIO32 (T9), confirmed by ESPHome config (threshold 1450 with IIR filter).
#define PIN_TOUCH        32
// Touch detection is CALIBRATED AT BOOT: threshold = touchRead(pin) - this margin.
// See Touch::begin().
//
// SCALE CHANGED WITH THE CORE. Arduino core 2.x touchRead() returned a divided-down value:
// untouched ~96, finger on ~74-75 on this board, and the margin was 8 (8.3% of baseline).
// Core 3.x (ESP-IDF 5) returns the peripheral's raw scale - the same one ESPHome reads -
// and this board's baseline measured 1572 at boot. The margin keeps the SAME PROPORTION of
// baseline as before (~8%), which also lands the derived threshold right at ESPHome's
// proven 1450 for this hardware: 1572 - 120 = 1452.
#define TOUCH_BASELINE_MARGIN 120
// Fallback only, used if the boot baseline read looks bogus. ESPHome's threshold for this
// pad on the raw scale, proven in its config with the IIR filter enabled.
#define TOUCH_THRESHOLD  1450
// Long-press threshold.
#define TOUCH_LONG_MIN   500  // ms - long press
// Minimum press duration for a release to count as ANY event:
//     duration = now - pressStart;  if (duration >= this) { fire callbacks }
// This was once set to 119 ms, a value that gates ONLY the long-press handler and not
// clicks - which is why short taps were being dropped.
#define TOUCH_PRESS_DEBOUNCE_MS 35
// Window after a release to wait for a second tap before committing the click count
// (single vs double):
//     if (window < now - lastPressStart) finalize click count
// Tuned against observed behaviour: at 400 ms (the OneButton-family default) ordinary taps at a natural
// cadence merged into DOUBLE events, and at 350 ms double-tap was hard to trigger.
// 200 ms makes single taps reliable - which is the primary navigation on the real
// device ("tap and the app list scrolls one by one") - while a deliberate quick
// double still registers. Single = next, double = previous.
#define TOUCH_CLICK_WINDOW_MS 200
// Input is ignored for this long after touch init, while the pad calibrates its baseline
// ("Don't Touch Top!").
#define TOUCH_BOOT_GUARD_MS 2500

// -- LittleFS config paths -----------------------------------------------------
// Partition label in flash: "spiffs" (legacy label reused for LittleFS), mounted with
// LittleFS.begin(true, "/littlefs", 10, "spiffs").
#define SYS_DIR         "/.sys"
// The OTA page, when the filesystem carries one. NOT under /.sys - it is part of the web UI,
// served by GET /update in preference to the copy compiled into .rodata. That built-in copy is
// the fallback for a device whose filesystem is empty or broken, which is the one state where
// an upload form matters most.
#define PATH_UPDATE_HTML "/update.html"

#define PATH_APP        "/.sys/app.json"
#define PATH_CONFIG     "/.sys/config.json"
#define PATH_COIN2      "/.sys/coin2.json"
#define PATH_STOCK2     "/.sys/stock2.json"
#define PATH_CITY       "/.sys/city.json"
#define PATH_KEY        "/.sys/key.json"
#define PATH_UNIT       "/.sys/unit.json"
#define PATH_WEATHER_SCREEN "/.sys/weather_screen.json"
#define PATH_MONITOR    "/.sys/monitor.json"
#define PATH_BRT        "/.sys/brt.json"
#define PATH_TIMEBRT    "/.sys/timebrt.json"
#define PATH_NTP        "/.sys/ntp.json"
#define PATH_TZ         "/.sys/tz.json"
#define PATH_DAY        "/.sys/day.json"
#define PATH_HOUR12     "/.sys/hour12.json"
#define PATH_TIMECOLOR  "/.sys/timecolor.json"
#define PATH_DELAY      "/.sys/delay.json"
#define PATH_ALBUM      "/.sys/album.json"
#define PATH_GIF        "/.sys/gif.json"
#define PATH_BILI       "/.sys/bili.json"
#define PATH_SEP        "/.sys/sep.json"
#define PATH_CCOLOR     "/.sys/ccolor.json"
#define PATH_C_THEME    "/.sys/c_theme.json"
#define PATH_G_THEME    "/.sys/g_theme.json"
#define PATH_C_CHART    "/.sys/c_chart.json"
#define PATH_ST_CHART   "/.sys/st_chart.json"
#define PATH_CG_RANGE   "/.sys/cg_range.json"
// cgkl.json and cgk.json are DISTINCT files. cgk.json is the CoinGecko app's MAIN config
// (coin IDs gin0..gin11, g_i, g_theme, ...); cgkl.json is the coin list / logo URLs.
#define PATH_CGK        "/.sys/cgk.json"     // CoinGecko main config
#define PATH_CGKL       "/.sys/cgkl.json"    // CoinGecko coin list / logo URLs
#define PATH_CK         "/.sys/ck.json"     // Yahoo COOKIE, key "ck"
#define PATH_CRU        "/.sys/cru.json"    // Yahoo CRUMB,  key "cu"
#define PATH_CR         "/.sys/cr.json"     // Binance rotation interval
#define PATH_K          "/.sys/k.json"      // Binance kline interval
#define PATH_STK        "/.sys/stk.json"    // Stock kline interval
#define PATH_CG_CHART   "/.sys/cg_chart_"  // prefix; append the coin id (range is in the header)

// Backlight duty when brt.json is absent or unreadable. RAW and ACTIVE-LOW: higher is
// dimmer. Display.h's _dayBright starts from this too, so the default has one definition.
#define BRT_DEFAULT 180

// How long the device must run before a boot counts as HEALTHY and the boot-loop markers
// (OTA_FLAG_R1/R2) are cleared. See the guard in main.cpp step 8 and the reset in
// AppCtrl::runLoop. 30s is comfortably past the risky part of startup -- WiFi, NTP, the
// first app's onEnter and its first fetch -- so a crash inside any of those still counts
// against the guard, while an ordinary session resets it.
#define BOOT_HEALTHY_MS 30000UL

// Ceiling for the boot countdown in delay.json, in seconds.
//
// WHAT THE SETTING IS FOR IS NOT DOCUMENTED anywhere, so the purpose below is INFERRED from
// where it runs, not established:
//   * it runs AFTER WiFi.begin() and BEFORE the connect polling, so it delays the VERDICT
//     on the connection rather than the attempt -- association proceeds during the wait;
//   * failing that verdict drops the device into AP mode, needing manual recovery;
//   * the web UI names the field wifiDelay, places it under the password, suggests 30.
// Best reading: "wait longer before declaring the WiFi dead", e.g. while a router that
// shares a power cut finishes booting. Treat that as a working theory.
//
// The countdown loop itself is unbounded - `while (secs != 0) { show "Delay <n>"; delay
// 1000; secs--; }` - and the UI field has no max, so 9999 would leave a device on a countdown
// screen for 2h46m with no way in. Hence a ceiling here.
//
// That was harmless only while the value never arrived: it is stored as a JSON *string* and
// the readers used ArduinoJson's `| default`, which discards strings, so the countdown was
// always 0. Reading it correctly makes the hazard real, so the bound lands in the same
// change. 60 is chosen on risk asymmetry rather than on any documented range: too short
// merely falls back to AP mode, while too long looks like a dead device. It is double the
// UI's own suggested 30, and a clamp is logged, so a genuine need for longer shows up as a
// log line instead of silently not working.
#define BOOT_DELAY_MAX_SECS 60

// -- App identifiers -----------------------------------------------------------
#define APP_COIN        "app_bitcoin"
#define APP_COINGECKO   "app_coingecko"
#define APP_STOCK       "app_stock"
#define APP_WEATHER     "app_weather"
#define APP_PICTURE     "app_picture"
#define APP_MONITOR     "app_monitor"
#define APP_CLOCK       "app_clock"
#define APP_IDEA        "app_idea"
#define APP_SETTINGS    "app_settings"

// -- Web server routes -------------------------------------------------------------
#define ROUTE_FILELIST  "/filelist"     // GET  - list /image/ files
#define ROUTE_DELETE    "/delete"       // POST - delete file from /image/
#define ROUTE_SET       "/set"          // POST - update any /.sys/ config key
#define ROUTE_V_JSON    "/v.json"       // GET  - firmware version JSON
#define ROUTE_WIFI_JSON "/wifi.json"    // GET  - current WiFi status + SSID list
#define ROUTE_WIFISAVE  "/wifisave"     // POST - save WiFi credentials (fields: s,p)
#define ROUTE_SPACE     "/space.json"   // GET  - LittleFS usage stats
#define ROUTE_UPLOAD    "/doUpload"     // POST multipart - upload file to /image/
#define ROUTE_UPDATE    "/update"       // GET  - OTA update UI (gzip embedded HTML)

// -- DEVICE IDENTITY - the single source of truth ------------------------------
// Everything user-visible that names this device derives from here. It used to be spread
// across five files: the AP SSID alone was defined three times (Config.h, WiFiMgr.h and a
// bare literal in main.cpp), so a rename could leave the captive-portal network called one
// thing while the screen told you to look for another.
#define DEVICE_NAME     "SmolTV PRO"    // boot/status screen title, and the AP name
#define DEVICE_MODEL    "SmolTV-PRO"    // model reported by /v.json
#define MDNS_HOST       "smoltv"        // http://smoltv.local
#define HTTP_USER_AGENT "SmolTV-Pro"    // sent on outbound HTTP requests

// AP captive-portal: deliberately the same string the screen shows, so what the user is
// told to look for is exactly what gets advertised.
#define AP_SSID         DEVICE_NAME     // open network - softAP is called without a password
#define AP_URL          "http://192.168.4.1"

// WiFi scan parameters, shared by BOTH scans: the async one the city save kicks off so the
// wizard's step 3 has a list ready, and the synchronous /wifi.json. Identical on purpose - a
// caller that waits for the other's results has to know how long they take. 13 channels x
// 300 ms is about 4 s; an ACTIVE scan showing hidden networks can take 8 s, which outlasted
// that wait and reported an empty list while the scan was still running.
#define SCAN_PASSIVE        true
#define SCAN_SHOW_HIDDEN    false
#define SCAN_MS_PER_CHANNEL 300

// OTA partition-select flags (written by OTA handler, removed on next boot)
#define OTA_FLAG_R1     "/r1.flag"
#define OTA_FLAG_R2     "/r2.flag"

// Firmware version string - the ONE identity of a build. Reported by /v.json, shown on the
// boot splash and on the Settings screen, and printed on the boot line, all from this macro
// so they cannot drift apart.
//
// BUMP THIS ON EVERY CHANGE, semantically: patch for a fix, minor for a feature.
//
// It is also how a flash is verified. That verification matters: `__DATE__ __TIME__` cannot
// do it, because PlatformIO only recompiles main.cpp when ITS dependencies change, so editing
// an app header leaves the stamp identical even though the image is new - which has already
// cost real debugging time. And the check must compare for EQUALITY with the expected new
// version, never merely "the device answered": a poll issued right after an OTA is routinely
// answered by the PRE-reboot firmware, which reads as a failed flash if you accept the first
// response. Poll /v.json until "v" equals what you just built.
//
// This is deliberately the ONLY build identity. A separate counter and its own route used
// to exist alongside it, which is two versions of truth that can disagree.
#define FW_VERSION      "V1.9.16EN"

// -- Display colour markup -----------------------------------------------------
// Text segments use the format  #RRGGBB text#
// where RRGGBB is a 6-digit hex colour.

// -- Default NTP servers -------------------------------------------------------
#define NTP_DEFAULT     "pool.ntp.org"
#define NTP_ALT1        "ntp.aliyun.com"
#define NTP_ALT2        "time.windows.com"

// -- HTTP user-agent -----------------------------------------------------------
// Sent on the CoinGecko requests. Note it really does end "Safari/537.3", truncated by one
// character, and pins Chrome/58. Do NOT "correct" either: server-side treatment can depend
// on the exact UA string.
#define HTTP_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.3"

// -- API base URLs -------------------------------------------------------------
#define URL_BINANCE_KLINES  "http://data-api.binance.vision/api/v3/uiKlines?symbol="
#define URL_BINANCE_BATCH   "http://data-api.binance.vision/api/v3/ticker/24hr?symbols="
#define URL_COINGECKO_PRICE "https://api.coingecko.com/api/v3/simple/price?ids="
#define URL_COINGECKO_CHART "https://api.coingecko.com/api/v3/coins/"
#define URL_COINGECKO_CHART_SUFFIX "/market_chart?vs_currency=usd&days="
#define URL_YAHOO_CRUMB_FC  "https://fc.yahoo.com"
#define URL_YAHOO_CRUMB     "https://query1.finance.yahoo.com/v1/test/getcrumb"
#define URL_YAHOO_QUOTE     "https://query1.finance.yahoo.com/v7/finance/quote?&symbols="
// PLAIN HTTP, deliberately. The binary holds both forms but the https one
// has NO xrefs - it is dead. The referenced string is
//reached from the constant pool, and it is http://.
// That matters for more than fidelity: skipping TLS on this request removes the
// ~40 KB contiguous mbedTLS allocation that was making our chart fetch fail.
// NOTE the scheme: this endpoint takes plain http://, but Yahoo now 301-redirects it, so
// https is forced here. See fetchChart().
#define URL_YAHOO_CHART     "https://query1.finance.yahoo.com/v8/finance/chart/"
// Chart query fragments, in order, from the same constant pool:
// "?interval=" "&range="
// "&includePrePost=true"
#define URL_YAHOO_CH_IV     "?interval="
#define URL_YAHOO_CH_RANGE  "&range="
#define URL_YAHOO_CH_PREPOST "&includePrePost=true"
// Header values the chart request sends: User-Agent (note 6.0, not
// 5.0) and Connection: keep-alive.
#define YAHOO_CHART_UA      "Mozilla/6.0 (X11; Linux x86_64)"
#define URL_YAHOO_FIELDS    "&fields=currency,priceHint,marketState,regularMarketChange,regularMarketChangePercent,regularMarketPrice,preMarketChange,preMarketChangePercent,preMarketPrice,postMarketChange,postMarketChangePercent,postMarketPrice&crumb="
#define URL_OWM             "http://api.openweathermap.org:80/data/2.5/weather?q=%s&appid=%s&units=%s&lang=en"
#define URL_OWM_ID          "http://api.openweathermap.org:80/data/2.5/weather?id=%s&appid=%s&units=%s&lang=en"

// Open-Meteo's geocoder. Keyless, and plain HTTP works - no TLS handshake, no heap spike.
// Returns results[0].{latitude,longitude,name,country_code} in about 400 bytes for count=1.
#define URL_OPEN_METEO_GEO  "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json"

// The city a device shows before anyone configures one.
//
// A NAME, deliberately, not the numeric OpenWeather id 1007311 that means the same place. cd1
// holding a name is what makes the KEYLESS path work out of the box: the geocoder resolves a
// name to coordinates, and there is nothing it can do with an id. With a key, a name queries
// OpenWeather's ?q= form just as happily.
#define WEATHER_DEFAULT_CITY "Durban"

// -- Filesystem helpers --------------------------------------------------------
namespace FSUtil {

// -- ONE LOCK OVER LITTLEFS, and why it is not optional ---------------------------
// Reproduced 2026-07-30 while sweeping the CoinGecko web UI:
//
//   assert failed: lfs_file_read lfs.c:6000 (lfs_mlist_isopen(lfs->mlist, ...))
//   ... vfs_littlefs_read <- fread <- File::read <- FSUtil::readFile
//       <- handleSetCore <- SyncWeb::h_set          (the HTTPD task, core 0)
//
// i.e. the web task was part-way through reading a config file when the MAIN task ran its
// own LittleFS operation (AppCoinGecko's chart-cache write - the log line immediately before
// the abort is "[cg] chart 31 points"). littlefs then found the open-file handle missing from
// its mlist and aborted the whole device. Two tasks, one filesystem, no mutual exclusion.
//
// It reproduced on a /set save, so it is on the exact path the web UI drives: settle a save
// while an app happens to touch the FS and the device reboots. Concurrent READS had survived
// heavy hammering before (264 file fetches under the Picture app), which is why this went
// unnoticed - it needs a WRITE to race a read.
//
// A recursive mutex, because writeJson -> writeFile and readJson -> readFile both nest.
//
// COVERAGE, stated honestly: FSUtil (all config I/O), the CoinGecko chart cache, SyncWeb's
// static file serving and /delete. NOT the multipart upload path, and NOT the GIF file handles
// that AppPicture/AppWeather keep open across frames - both hold a file open for far too long
// to sit under one lock (an upload can run seconds, a GIF for as long as it plays), and the
// Guard below deliberately proceeds after a 3 s timeout rather than deadlocking the render
// loop. Those remain a theoretical race; the demonstrated one is closed.
inline SemaphoreHandle_t _fsMutex = nullptr;
inline void lockInit() { if (!_fsMutex) _fsMutex = xSemaphoreCreateRecursiveMutex(); }
struct Guard {
    bool held = false;
    Guard()  { lockInit(); if (_fsMutex) held = xSemaphoreTakeRecursive(_fsMutex, pdMS_TO_TICKS(3000)) == pdTRUE; }
    ~Guard() { if (held) xSemaphoreGiveRecursive(_fsMutex); }
};

inline String readFile(const char* path) {
    Guard g;
    // A MISSING config file is the NORMAL case: none are shipped, and one is created only
    // when the user saves that app's settings, so a lightly-used device has just a handful.
    // arduino-esp32 logs an ESP_LOGE from vfs_api on the
    // failed open, and an exists() pre-check does NOT avoid it because VFSImpl::exists()
    // opens the file too. The tag is therefore silenced once in setup() - see main.cpp.
    File f = LittleFS.open(path, "r");
    if (!f) return "";
    String s = f.readString();
    f.close();
    return s;
}

inline bool writeFile(const char* path, const String& content) {
    Guard g;
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    f.print(content);
    f.close();
    return true;
}

inline bool readJson(const char* path, JsonDocument& doc) {
    String raw = readFile(path);
    if (raw.isEmpty()) return false;
    DeserializationError err = deserializeJson(doc, raw);
    return err == DeserializationError::Ok;
}

inline bool writeJson(const char* path, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    return writeFile(path, out);
}

// String-or-number tolerant integer read. The real web UI writes config values as JSON
// NUMBERS (`"g_rot_i":8`), which `doc[k] | def` reads fine. But a value seeded through the
// config form POST is stored as a STRING (`"g_rot_i":"8"`), and ArduinoJson's
// `doc[k] | def` does NOT coerce a string to int - it silently returns `def`, so a
// form-configured interval was ignored and the app fell back to its 60 s default. This reads
// either representation: a real int/float as-is, a numeric string via atoi, else def.
inline int readInt(JsonDocument& doc, const char* key, int def) {
    JsonVariantConst v = doc[key];
    if (v.is<int>() || v.is<float>()) return v.as<int>();
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        return (s && *s) ? atoi(s) : def;
    }
    return def;
}

} // namespace FSUtil

// app.json's real schema is exactly { "theme": <index>, "apps": { <9 flags> } }.
//
// Older revisions of this firmware also wrote "rot_i" (the active app's NAME, from a
// Scheduler class that no longer exists) and "delay" (which belongs in delay.json). Neither
// key belongs in this file and nothing reads them now - but every writer here reads the file
// and writes it back to preserve the "apps" flags, so the orphans survived indefinitely on
// any device that had once run those revisions.
//
// Call this before writing app.json so they are dropped on the next save.
inline void appJsonDropOrphanKeys(JsonDocument& doc) {
    doc.remove("rot_i");
    doc.remove("delay");
}
