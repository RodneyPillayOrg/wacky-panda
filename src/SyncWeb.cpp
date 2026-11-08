#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <Update.h>          // OTA sink for /update (moved off the async :8080 server)
#include <esp_image_format.h>   // ESP_IMAGE_HEADER_MAGIC - the app image's 0xE9
#include "Config.h"
#include "Log.h"
#include "HttpParams.h"
#include "WiFiMgr.h"
#include "WebRoutes.h"      // handleSetCore - the shared /set config contract
#include "SyncWeb.h"

// -- SyncWeb - the bomb-proof HTTP server (port 80) -------------------------------
//
// WHY THIS EXISTS. The web UI loads ~12 GIF/JPGs (300-420 KB each) plus a 118 KB gzip
// page. Served by ESPAsyncWebServer that reliably wrecked the device, and the cause was
// TWO compounding heap problems, both measured:
//
//  1. AsyncAbstractResponse::_ack() malloc()s a TCP-window-sized buffer
//     (CONFIG_LWIP_TCP_SND_BUF_DEFAULT = 5744) on EVERY ack, for EVERY connection, then
//     frees it. Six connections => six interleaved variable-size alloc/free cycles.
//  2. lwIP on ESP-IDF has NO memory pools: port/esp32/include/lwipopts.h sets
//     MEM_LIBC_MALLOC=1 and MEMP_MEM_MALLOC=1, and PBUF_POOL_SIZE is not defined at all.
//     Every pbuf and every PBUF_RAM copy on send() comes from the general heap. Per
//     Espressif's own formula that is (tcp_tx_win + tcp_rx_win) = 5744+5744 ~= 11.5 KB of
//     heap PER ACTIVE TCP CONNECTION, arriving in ~1.4 KB fragments.
//
// Net effect measured on-device: the largest contiguous 8-bit block collapsed from ~70 KB
// to ~4 KB while TOTAL free stayed ~60 KB. Fragmentation, not exhaustion - which is why
// it locked up and why the Picture app's 38 KB band then failed to allocate.
//
// (2) is unfixable by any server choice: there is no Kconfig in IDF 4.4 or 5.x to move
// lwIP onto pools, and arduino-esp32 ships lwIP prebuilt so -D flags cannot change the
// window sizes. Therefore CONCURRENCY is the dominant term, and the fix is to bound it.
//
// WHAT THIS IS. esp_http_server (ESP-IDF's own httpd, already in our framework - no
// version bump, no TLS risk). Its memory model is the opposite of the async server's:
// everything is calloc()'d ONCE at httpd_start(), the per-socket buffer is a 128-byte
// array embedded in struct sock_db, and there is exactly ONE ~1 KB request scratch buffer
// for the whole server (not per connection) because a single task serves one request at a
// time in a select() loop. There is no ack callback and so no per-ack malloc.
//
// The shape to aim for is a synchronous socket server serving static files through one
// fixed 1360-byte buffer: bounded memory regardless of file size or how many connections the
// browser opens.
namespace SyncWeb {

// -- Tunables (every value justified) --------------------------------------------
// MUST be >= the browser's per-host connection count (6). This is httpd's own default.
//
// Getting this wrong was the subtlest bug of the whole exercise, in BOTH directions:
//   * Too low and httpd answers 503 when it runs out of slots. That is fatal here, because
//     a browser NEVER retries a failed <img> - at 2 the page's own opening API burst
//     (album.json + space.json + filelist, issued concurrently) got a 503 and the gallery
//     rendered EMPTY; at 4 the filelist survived but images 5..12 were refused and every
//     thumbnail stayed broken, even though fetching those exact URLs returned 200 with
//     correct bytes. lru_purge_enable does NOT save you: purging an LRU session that is
//     mid-send fails, and httpd falls back to 503.
//   * Too low ALSO doesn't buy the memory it appears to. I had assumed each accepted socket
//     costs the full ~11.5 KB of lwIP window. It does not: esp_http_server is SINGLE-TASK
//     and serves strictly one request at a time, so only ONE socket ever has a full send
//     buffer. The others are accepted-but-idle and cost about a PCB (~200 bytes) each.
//
// So the cap was never the thing protecting the heap - bounded per-request buffering (the
// 2 KB static buffer below) and removing the duplicate band buffer were. Keeping the cap
// generous costs ~1 KB and eliminates the refusals entirely.
static constexpr uint16_t MAX_SOCKETS   = 7;
//
// NOT DONE, deliberately: shrinking the per-socket send buffer with setsockopt(SO_SNDBUF)
// (default CONFIG_LWIP_TCP_SND_BUF_DEFAULT = 5744) would save a further ~3 KB per
// connection, but including <sys/socket.h> in this TU fails to compile (lwip/ip4_addr.h:
// "expected ')' before numeric constant" - something in our build expands the u32_t token,
// and it reproduces regardless of include order). It is also unverified whether lwIP on
// IDF 4.4 honours SO_SNDBUF at all. MAX_SOCKETS is the certain lever and already caps
// total exposure, so this is left on the table rather than chased.
//
// The browser's extra connections queue in the TCP backlog instead of being refused -
// they simply wait, which is transparent to the browser. Refusing
// (a short backlog) is what makes a <img> show as broken, so keep this >= the browser's
// per-host connection count.
static constexpr uint16_t BACKLOG       = 6;
// One static send buffer in .bss. Legal precisely because httpd is single-task: only one
// handler runs at a time, so one buffer cannot be contended. This takes application-side
// heap traffic for file serving to exactly ZERO. Same discipline as the 240x80 display
// band.
//
// 1360 is not arbitrary: it sits just under one MSS (CONFIG_LWIP_TCP_MSS = 1436) so each
// send() maps to
// exactly ONE pbuf. It was 2048, which straddles the MSS boundary and therefore asks lwIP
// for two allocations per chunk - the second of which is what failed under fragmentation.
static constexpr size_t   SEND_BUF_SIZE = 1360;
// -- Streaming buffer: an EXACT MULTIPLE of the MSS chunk -------------------------
// Measured baseline before this change: 12 gallery files, 2,271 KB, 11.9 s = 190 KB/s. The
// cost was per-call overhead, not the filesystem - one f.read() plus one
// httpd_resp_send_chunk() (plus its chunked-encoding header and a socket write) for every
// 1,360 bytes means ~1,700 round trips through that path for a single 2.2 MB gallery.
//
// The fix keeps the property that actually mattered and drops the one that did not. What
// mattered: every send must map to a WHOLE number of full pbufs, with no odd remainder - a
// 2,048-byte chunk straddling the 1,436-byte MSS is what fragmented the heap and produced the
// aborts. What did not matter: the send being exactly ONE pbuf. So this is 3 x SEND_BUF_SIZE:
// uniform, remainder-free, and a third of the syscalls. Keep any future value an exact
// multiple of SEND_BUF_SIZE - that invariant is the whole protection.
static constexpr size_t   STREAM_MULT     = 3;
static constexpr size_t   STREAM_BUF_SIZE = SEND_BUF_SIZE * STREAM_MULT;   // 4080
static uint8_t s_sendBuf[STREAM_BUF_SIZE];

// -- Flow control: the fix for "images break while the Picture app is running" --------
// MEASURED FAILURE (serial, Picture active, 6-way parallel hammer of 12 media files):
//     [heap] free=54388 blk8=4084  app=app_picture mode=active
//     [heap] free=48824 blk8=1332  app=app_picture mode=active
//     [sync] abort /image/ezgif-7-b5f43.gif after 100352 bytes
//     [sync] abort /image/ezgif-8b226f9c0a4de5.gif after 4096 bytes
// 48 KB free but only 1.3 KB CONTIGUOUS. The GIF decoder plus up to MAX_SOCKETS worth of
// unacked TCP data fragments the heap, lwIP cannot allocate a send pbuf, and
// httpd_resp_send_chunk fails. The old code logged that as "client aborted" and returned
// ESP_FAIL - so a transient, self-healing memory shortage permanently broke the image. A
// browser never retries a failed <img>, which is why it looked like a dead server.
//
// There is no reboot, no WDT and no panic involved; the app loop keeps running throughout.
//
// WHAT ACTUALLY FIXED IT - recorded honestly, because it is not what it looks like:
// the real fix was SEND_BUF_SIZE 2048 -> 1360 above. Uniform one-MSS chunks make every
// lwIP send allocate exactly one same-sized pbuf, which stops the odd-sized-remainder
// fragmentation the 2048 chunks were causing. Measured over 264 file fetches with Picture
// active (6-way and 12-way parallel, every byte md5-verified against a baseline):
//     before: blk8 floor 1332, aborts on 3-9 of every 120 requests
//     after:  blk8 floor 7668, 0 failures, and the gate below NEVER FIRED
//
// The gate is therefore insurance, not the cure, and its wait path is UNEXERCISED by any
// test so far - treat it as untested code. It is kept because the floor it defends is only
// ~1.5 KB above one pbuf, and a future app that fragments harder than the GIF decoder
// would otherwise resurrect the silent-corruption failure mode.
//
// Deliberately a pre-send gate and not a retry-after-failure: httpd's httpd_send_all may
// already have written part of a chunk before returning an error, so re-sending that chunk
// would duplicate bytes and corrupt the file.
static constexpr size_t   SEND_FLOOR_BLK8 = 6 * 1024;   // ~4x one pbuf, measured-safe
static constexpr uint32_t SEND_WAIT_STEP_MS = 20;
static constexpr uint32_t SEND_WAIT_MAX_MS  = 4000;     // cap: prefer slow over hung

// Returns the blk8 we ended up with (for logging); never blocks longer than SEND_WAIT_MAX_MS.
static size_t waitForSendRoom() {
    size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t waited = 0;
    while (blk < SEND_FLOOR_BLK8 && waited < SEND_WAIT_MAX_MS) {
        vTaskDelay(pdMS_TO_TICKS(SEND_WAIT_STEP_MS));
        waited += SEND_WAIT_STEP_MS;
        blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }
    return blk;
}

// CONFIG_HTTPD_MAX_URI_LEN is 1024/512 in the prebuilt lib and cannot be raised, so a
// very long /set batch would 414. Worst realistic case (BitCoin: 12 ids + 12 names +
// flags) measures ~405 chars, but a user with long custom coin names could approach the
// limit - warn loudly rather than fail mysteriously.
static constexpr size_t   QUERY_BUF_SIZE = 640;

static httpd_handle_t s_server = nullptr;

// -- Helpers ---------------------------------------------------------------------
static const char* contentTypeFor(const String& p) {
    if (p.endsWith(".html")) return "text/html";
    if (p.endsWith(".css"))  return "text/css";
    if (p.endsWith(".js"))   return "application/javascript";
    // A STATIC .json file is served as text/plain: GET /.sys/day.json answers
    // "Content-Type: text/plain". Only the GENERATED json routes (/v.json, /space.json) use
    // "text/json", and they set it explicitly.
    if (p.endsWith(".json")) return "text/plain";
    if (p.endsWith(".gif"))  return "image/gif";
    if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
    if (p.endsWith(".png"))  return "image/png";
    if (p.endsWith(".bmp"))  return "image/bmp";
    if (p.endsWith(".ico"))  return "image/x-icon";
    if (p.endsWith(".svg"))  return "image/svg+xml";
    if (p.endsWith(".txt"))  return "text/plain";
    return "application/octet-stream";
}

// Path portion of the request URI, query stripped and percent-decoded.
static String pathOf(httpd_req_t* req) {
    const char* q = strchr(req->uri, '?');
    const char* end = q ? q : (req->uri + strlen(req->uri));
    return httpUrlDecode(req->uri, end);
}

// Parse the query string into a ReqParams. Empty if there is none.
static String queryOf(httpd_req_t* req) {
    const size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) return String();
    if (len >= QUERY_BUF_SIZE) {
        LOGF("[sync] WARNING query %u bytes exceeds buffer %u -- truncated",
             (unsigned)len, (unsigned)QUERY_BUF_SIZE);
    }
    char buf[QUERY_BUF_SIZE];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) return String();
    return String(buf);
}

static esp_err_t sendBody(httpd_req_t* req, int status, const char* ctype, const String& body) {
    char st[32];
    switch (status) {
        case 200: strcpy(st, "200 OK"); break;
        case 404: strcpy(st, "404 Not Found"); break;
        case 400: strcpy(st, "400 Bad Request"); break;
        case 500: strcpy(st, "500 Internal Server Error"); break;
        default:  snprintf(st, sizeof(st), "%d", status); break;
    }
    httpd_resp_set_status(req, st);
    httpd_resp_set_type(req, ctype);
    return httpd_resp_send(req, body.c_str(), body.length());
}

// -- Route: /set - the shared config contract ------------------------------------
static esp_err_t h_set(httpd_req_t* req) {
    const String q = queryOf(req);
    QueryParams P(q.c_str());
    const SetResult r = handleSetCore(P);
    return sendBody(req, r.status, r.contentType, r.body);
}

// -- Route: /space.json ----------------------------------------------------------
static esp_err_t h_space(httpd_req_t* req) {
    DynamicJsonDocument doc(128);
    // Keys are "total" and "free" - NOT "used": {"total":3866624,"free":1072652}.
    // The web UI reads data.free and derives the used
    // percentage from it, so reporting "used" instead left it reading undefined -> 0 and
    // displaying "Free 0 B / Used 100%" on a filesystem that was actually 73% full.
    doc["total"] = LittleFS.totalBytes();
    doc["free"]  = LittleFS.totalBytes() - LittleFS.usedBytes();
    String out; serializeJson(doc, out);
    // "text/json", not "application/json", for both /space.json and /v.json.
    return sendBody(req, 200, "text/json", out);
}

// -- Route: /v.json --------------------------------------------------------------
static esp_err_t h_vjson(httpd_req_t* req) {
    return sendBody(req, 200, "text/json",
                    "{\"m\":\"" DEVICE_MODEL "\",\"v\":\"" FW_VERSION "\"}");
}

// -- Route: /delete --------------------------------------------------------------
// The argument is "file", and the reply is status 200 "text/plain" with body "OK" or
// "Failed" - NOT "{}" as application/json, which the UI does not accept. ("name" is kept as
// an extra alias; harmless tolerance.)
//
// Deleting an image must also DROP THE PICTURE APP'S PINNED IMAGE, so the app never points
// at a file that no longer exists. If the deleted path starts with "/image/" and equals the
// stored path in /.sys/album.json, that key is written back empty and, when Picture is the
// current app, the cleared value is pushed into the LIVE app. The other album.json keys
// (gif_loop, i_i, autoplay) are preserved.
static esp_err_t h_delete(httpd_req_t* req) {
    const String q = queryOf(req);
    QueryParams P(q.c_str());
    String path;
    if (P.has("file"))      path = P.get("file");
    else if (P.has("name")) path = P.get("name");
    // "path" too. This is a real client shape - /delete?path=/.sys/k.json - and answering
    // "Failed" to it, because only file/name were checked, is a rejection of a valid
    // request.
    else if (P.has("path")) path = P.get("path");
    // Serialise with the main task; see FSUtil::Guard in Config.h.
    bool ok;
    { FSUtil::Guard fsg; ok = !path.isEmpty() && LittleFS.remove(path); }

    if (ok && path.startsWith("/image/")) {
        DynamicJsonDocument doc(256);
        if (FSUtil::readJson(PATH_ALBUM, doc)) {
            const String pinned = doc["path"] | "";
            if (!pinned.isEmpty() && pinned == path) {
                doc["path"] = "";                 // other keys preserved
                FSUtil::writeJson(PATH_ALBUM, doc);
                LOGF("[sync] deleted the pinned image - cleared album path");
                if (_appReloadFn) _appReloadFn();  // our equivalent of the live-app push
            }
        }
    }
    return sendBody(req, 200, "text/plain", ok ? "OK" : "Failed");
}

// -- Route: /filelist ------------------------------------------------------------
// The UI parses this HTML table with DOMParser. Emits FULL paths (<dir>/<basename>):
// arduino-esp32 2.x File::name() returns only the basename, and emitting that made every
// thumbnail 404. The "Set" column exists only for the weather GIF folder.
static esp_err_t h_filelist(httpd_req_t* req) {
    const String q = queryOf(req);
    QueryParams P(q.c_str());
    String dir = "/image";
    if (P.has("dir"))       dir = P.get("dir");
    else if (P.has("path")) dir = P.get("path");   // legacy compat

    const bool gifDir = (dir == "/image/80x80.gif");
    String html = "<table id='list'><tbody>";
    html += "<tr><th>#</th><th>File name</th><th>Size(KB)</th><th></th>";
    if (gifDir) html += "<th></th>";
    html += "</tr>";

    File root = LittleFS.open(dir);
    if (root && root.isDirectory()) {
        String base = dir;
        if (!base.endsWith("/")) base += "/";
        int n = 0;
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                n++;
                String name = String(f.name());
                const int slash = name.lastIndexOf('/');
                if (slash >= 0) name = name.substring(slash + 1);
                const String fullPath = base + name;
                html += "<tr><td>" + String(n) + "</td>";
                html += "<td><a href='" + fullPath + "'>" + name + "</a></td>";
                html += "<td>" + String((unsigned long)(f.size() / 1024)) + "</td>";
                html += "<td><input type='button' class='btndel' onclick=\"deletef('"
                      + fullPath + "')\" value='X'></td>";
                if (gifDir)
                    html += "<td><input type='button' class='btndel' onclick=\"setgif('"
                          + fullPath + "')\" value='Set'></td>";
                html += "</tr>";
            }
            f = root.openNextFile();
        }
    }
    html += "</tbody></table>";
    return sendBody(req, 200, "text/html", html);
}

// -- Route: catch-all static files -----------------------------------------------
// Streams ANY LittleFS file through the single static buffer. This is the path that used
// to fragment the heap; here peak application allocation is zero and lwIP's is bounded by
// MAX_SOCKETS. Covers the page, images, and the /.sys/*.json config reads (whose 404 for
// a missing file is load-bearing: the UI reads it as "this app was never configured").
static esp_err_t h_configJson(httpd_req_t* req);    // credential redaction, see below
static esp_err_t sendFsFile(httpd_req_t* req, const String& path);   // defined below

static esp_err_t h_static(httpd_req_t* req) {
    String path = pathOf(req);

    // "/" -> the setup wizard in AP mode, the full UI once joined.
    if (path == "/" || path.isEmpty()) {
        path = (WiFiMgr::isAP() && LittleFS.exists("/portal.html")) ? "/portal.html"
                                                                   : "/index.html";
    }
    if (path.endsWith("/")) path += "index.html";

    // Repeated slashes are one slash to LittleFS, so //.sys/x and /.sys/x open the same file.
    // Collapse them here so the credential test below sees the same path LittleFS will.
    while (path.indexOf("//") >= 0) path.replace("//", "/");

    // The credential file is served redacted, never from flash. This test lives HERE, in the
    // static handler, rather than as an exact route in the table, because httpd matches on
    // the RAW uri while pathOf() percent-decodes - so an exact route is trivially side-
    // stepped. Proven on the device: with the redaction registered as a route,
    // GET /.sys/config%2Ejson and GET //.sys/config.json both missed it, fell through to
    // "/*", and returned the plaintext password. Decoding first and testing once closes
    // every spelling of the path at the single point where it is finally resolved.
    if (path == PATH_CONFIG) return h_configJson(req);

    return sendFsFile(req, path);
}

// Stream one file from LittleFS. Split out of h_static so another handler can serve a
// filesystem file with exactly the same semantics - gzip encoding, cache policy, flow control -
// rather than a second, subtly different copy. h_updatePage uses it for /update.html.
static esp_err_t sendFsFile(httpd_req_t* req, const String& path) {
    // The 404 body is exactly "404: Not Found" - 14 bytes, text/plain. It was once
    // "Not found", which the UI does not expect.
    // Hold the FS lock for the WHOLE open-stream-close, not just the open. A file kept open
    // across many send() chunks while the main task writes to LittleFS is exactly the pairing
    // that aborted littlefs ("assert failed: lfs_file_read ... lfs_mlist_isopen") - see the
    // note by FSUtil::Guard. At 1360-byte chunks even a 100 KB asset holds it only briefly.
    FSUtil::Guard fsg;
    if (!LittleFS.exists(path)) {
        LOGF("[sync] 404 %s", path.c_str());
        return sendBody(req, 404, "text/plain", "404: Not Found");
    }
    File f = LittleFS.open(path, "r");
    if (!f) return sendBody(req, 500, "text/plain", "open failed");
    if (f.isDirectory()) { f.close(); return sendBody(req, 404, "text/plain", "404: Not Found"); }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, contentTypeFor(path));
    // The web UI is stored gzip-compressed in LittleFS - pages and the stylesheet alike - so
    // it must be served raw with the encoding header rather than decompressed here.
    if (path.endsWith(".html") || path.endsWith(".css"))
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    // Cache-Control:
    //   /image/*.gif|jpg -> "public, max-age=31536000, immutable"
    //   everything else  -> "Public"   (capital P is deliberate)
    // This is not cosmetic. Without the immutable one-year policy the browser re-downloads
    // every gallery image on every visit, which is a large part of why "#image" felt like it
    // hammered the device: the load we were fighting was partly self-inflicted. With it, a
    // normal revisit re-fetches nothing.
    const bool isMedia = path.startsWith("/image/");
    httpd_resp_set_hdr(req, "Cache-Control",
                       isMedia ? "public, max-age=31536000, immutable" : "Public");

    size_t sent = 0;
    uint32_t stalls = 0;
    uint32_t chunkIdx = 0;
    size_t   lastBlk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    while (true) {
        const int n = f.read(s_sendBuf, STREAM_BUF_SIZE);
        if (n <= 0) break;
        // Never hand lwIP a send it cannot satisfy - see the flow-control note above.
        //
        // Sampled, not per-chunk. heap_caps_get_largest_free_block() WALKS THE HEAP, and
        // calling it before every chunk put an O(free-blocks) scan in the middle of the hot
        // path - ~560 walks for one 2.2 MB gallery. The gate exists to catch a heap that has
        // collapsed, which is a condition lasting many milliseconds, so checking every 8th
        // chunk (~32 KB) detects it just as well and costs an eighth as much. The chunk about
        // to be sent is at most STREAM_BUF_SIZE, so a stale reading cannot overcommit by more
        // than that, and SEND_FLOOR_BLK8 already carries far more margin than 4 KB.
        if ((chunkIdx++ & 7) == 0) {
            lastBlk = waitForSendRoom();
            if (lastBlk < SEND_FLOOR_BLK8) stalls++;
        }
        const size_t blk = lastBlk;
        if (httpd_resp_send_chunk(req, (const char*)s_sendBuf, n) != ESP_OK) {
            // Either a genuine client abort (navigated away / hard-refresh mid-transfer)
            // or a send that failed anyway. Log blk8 so the two stay distinguishable:
            // a healthy blk8 here means the client really did go away.
            f.close();
            LOGF("[sync] abort %s after %u bytes (blk8=%u stalls=%u)",
                 path.c_str(), (unsigned)sent, (unsigned)blk, (unsigned)stalls);
            return ESP_FAIL;
        }
        sent += n;
    }
    if (stalls) LOGF("[sync] %s throttled %u chunks", path.c_str(), (unsigned)stalls);
    f.close();
    return httpd_resp_send_chunk(req, nullptr, 0);   // terminate the chunked response
}

// -- Route: /wifi.json -----------------------------------------------------------
// AP list for the setup page / the System tab's Wi-Fi section:
//   {"aps":[{"ss":"SSID","c":"ch","e":1,"r":rssi},...]}
// The reply is Content-Type: text/html and looks like
//   {"aps":[{"c":"1","ss":"SmolTV PRO","e":0,"r":100},{"c":"9","ss":"...","e":1,"r":90},...]}
// Four details, each of which was wrong at some point:
//
//  1. `r` IS A 0-100 QUALITY PERCENTAGE, NOT RAW RSSI:
//         RSSI <= -100        -> 0
//         RSSI >= -50         -> 100
//         otherwise           -> (RSSI + 100) * 2
//     Sending raw RSSI (e.g. -67) makes the UI's signal bars read from a negative number,
//     so they always show weakest/none.
//  2. AT MOST 20 APs. Unbounded also risks overflowing the JSON document and emitting
//     truncated JSON in a dense RF environment.
//  3. Content type is text/html, not application/json.
//  4. The scan is SYNCHRONOUS but BOUNDED: scanNetworks(async=0, show_hidden=0, passive=1,
//     max_ms_per_chan=300), rather than an async scan with hidden networks shown and an
//     up-to-8 s wait.
//
// On the blocking question: ~300 ms x 13 channels is ~4 s during which this single-task
// server serves nothing else. Bounded at 4 s beats the previous unbounded wait, and the
// endpoint is only hit by the setup/System page. scanDelete() brackets the scan so the
// results are not left occupying heap afterwards.
// How long to wait for a scan already in flight. Twice the ~4 s a bounded scan takes (see the
// SCAN_* parameters in Config.h), so a timeout means something is genuinely wrong rather than
// merely slow.
static constexpr unsigned long SCAN_WAIT_MS = 8000;

static esp_err_t h_wifiJson(httpd_req_t* req) {
    const unsigned long t0 = millis();

    // DO NOT start blind. Saving the city in the setup wizard kicks off an ASYNC scan so that
    // step 3's list is ready, and starting a second scan while that one runs returns
    // WIFI_SCAN_RUNNING (-1), which this handler used to flatten to zero - so the portal
    // reported "No network found" the instant the city was saved, every time.
    //
    // So: adopt a finished scan's results, wait for one still running, and only start our own
    // when there is nothing to adopt.
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING &&
               millis() - t0 < SCAN_WAIT_MS) {
            delay(100);
        }
        LOGF("[sync] /wifi.json adopted async scan n=%d after %lums", n, millis() - t0);
    }
    // Start our own ONLY when nothing is pending. Starting one while a scan runs returns
    // WIFI_SCAN_RUNNING again, which is how a timed-out wait turned into "no networks found"
    // even though the scan was moments from finishing - and the next click then showed the
    // full list instantly, because by then the results were sitting there to be adopted.
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanDelete();
        n = WiFi.scanNetworks(false /*async*/, SCAN_SHOW_HIDDEN, SCAN_PASSIVE,
                              SCAN_MS_PER_CHANNEL);
    }
    if (n < 0) {
        LOGF("[sync] /wifi.json scan unfinished after %lums, reporting empty", millis() - t0);
        n = 0;
    }
    if (n > 20) n = 20;
    LOGF("[sync] /wifi.json aps=%d took=%lums", n, millis() - t0);

    DynamicJsonDocument doc(4096);
    JsonArray aps = doc.createNestedArray("aps");
    for (int i = 0; i < n; i++) {
        const int32_t rssi = WiFi.RSSI(i);
        int quality = 0;
        if (rssi > -100) quality = (rssi < -50) ? (int)((rssi + 100) * 2) : 100;
        JsonObject ap = aps.createNestedObject();   // key order matters: c, ss, e, r
        ap["c"]  = String(WiFi.channel(i));         // channel is a STRING
        ap["ss"] = WiFi.SSID(i);
        ap["e"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
        ap["r"]  = quality;
    }
    String out; serializeJson(doc, out);
    WiFi.scanDelete();
    return sendBody(req, 200, "text/html", out);
}

// -- Route: /.sys/config.json ----------------------------------------------------
// DELIBERATE DEVIATION, and the only path this server does not serve verbatim from flash.
// /.sys/config.json is the WiFi credential file, {"a":"ssid","p":"pass"}. Served as an
// ordinary static file it would hand the password to anyone who can reach the device, in
// plain text over unauthenticated HTTP, and into any filesystem backup taken over the wire.
// This handler blanks the password on the way out.
//
// The SSID is NOT blanked - it is not a secret, it is broadcast by the access point, and
// the settings page is far more useful when it can still tell you which network the device
// is on. Only "p" is withheld.
//
// The web UI prefills its WiFi form from this response and posts those field values back to
// /wifisave, so the empty password field is what the user would be sending if they saved
// without typing. Its own guard usually stops them: saveWifi() refuses to send when the
// selected network is encrypted and the password is under 8 characters. That check keys off
// the /wifi.json scan list, so it cannot fire for a network the scan did not see - on such a
// network, pressing Save Wi-Fi without typing a password would store an empty one. The fix
// is to retype the password, exactly as it would be for any other network change.
//
// Keys other than "p" are passed through untouched, so this stays correct if the file ever
// gains one. NOT registered as a route - h_static dispatches here after decoding the path,
// which is the only place every spelling of it converges; see the note there. Nothing on the
// device depends on reading credentials over HTTP - the firmware reads the file straight
// from LittleFS in BootConfig::loadWiFiConfig.
static esp_err_t h_configJson(httpd_req_t* req) {
    DynamicJsonDocument doc(512);
    if (!FSUtil::readJson(PATH_CONFIG, doc)) doc.to<JsonObject>();
    if (!doc.containsKey("a")) doc["a"] = "";
    doc["p"] = "";
    String out; serializeJson(doc, out);
    // text/plain is what contentTypeFor gives a static .json, so the reply is otherwise
    // indistinguishable from the one this path used to produce.
    return sendBody(req, 200, "text/plain", out);
}

// -- Route: /wifisave ------------------------------------------------------------
// ROUTING PARITY ONLY - the credential-saving BEHAVIOUR is deliberately untouched (it was
// explicitly excluded from this work). Moving the UI to :80 made the existing endpoint
// unreachable, so this restores reachability by calling exactly the same WiFiMgr path the
// async server used; no logic, params or reboot semantics were changed.
// The real UI does GET /wifisave?s=SSID&p=PASS; the fallback PROGMEM form posts ssid/pass.
static esp_err_t h_wifiSave(httpd_req_t* req) {
    const String q = queryOf(req);
    QueryParams P(q.c_str());
    String ssid, pass;
    if (P.has("s"))    ssid = P.get("s");
    if (P.has("p"))    pass = P.get("p");
    if (ssid.isEmpty() && P.has("ssid")) ssid = P.get("ssid");
    if (pass.isEmpty() && P.has("pass")) pass = P.get("pass");

    // REFUSE AN EMPTY SSID. This is not defensive padding - it is the fix for a real
    // incident: this route is reachable by GET (the UI does GET /wifisave?s=..&p=..),
    // so a single parameterless GET /wifisave reached saveCredentials("", ""), which
    // overwrites /.sys/config.json with {"a":"","p":""} and thereby DESTROYS the device's
    // Wi-Fi credentials. The device then rebooted into AP mode and fell off the network
    // entirely - recoverable only via the AP portal or a serial reflash.
    // Before: [wifi] file ssid='<your-ssid>'   After: [wifi] file ssid=''
    // The async server on :8080 always had the same hole; it just never got probed there.
    // A save with no SSID has no legitimate meaning, so reject it instead of persisting it.
    if (ssid.isEmpty()) {
        LOGF("[sync] /wifisave REFUSED (empty ssid) - credentials left untouched");
        return sendBody(req, 400, "text/plain", "ssid required");
    }
    // AN EMPTY PASSWORD FOR THE NETWORK ALREADY STORED MEANS "UNCHANGED", NOT "CLEAR IT".
    //
    // Second deviation of the same kind as the SSID guard above, and for the same reason.
    // Since the credential route stopped serving the password, the UI's password field
    // arrives empty while the SSID field arrives filled, so pressing Save Wi-Fi without
    // typing anything sends the real SSID and a blank password. The UI is supposed to stop
    // that - it refuses to send when the chosen network is encrypted and the password is
    // under 8 characters - but that check keys off the /wifi.json scan list, so it cannot
    // fire for a network the scan did not return, and the request goes out. Landing it
    // would blank a working password and drop the device into AP mode on reboot.
    //
    // Only the exact accident is caught: same SSID, no password. Everything legitimate still
    // works, because a real change types at least one of the two. Joining a genuinely open
    // network is a different SSID with no password, which passes straight through.
    //
    // Known limitation, accepted: taking the SAME network from encrypted to open cannot be
    // expressed, since it is indistinguishable from the accident. Factory reset, or save the
    // new network under its new name first.
    if (pass.isEmpty()) {
        DynamicJsonDocument cur(512);
        if (FSUtil::readJson(PATH_CONFIG, cur)) {
            const String storedSsid = cur["a"] | "";
            const String storedPass = cur["p"] | "";
            if (!storedPass.isEmpty() && ssid == storedSsid) {
                pass = storedPass;
                LOGF("[sync] /wifisave: same ssid, no password given - keeping the stored one");
            }
        }
    }

    WiFiMgr::saveCredentials(ssid, pass);
    const esp_err_t r = sendBody(req, 200, "text/plain", "Saved. Rebooting...");
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        ESP.restart();
        vTaskDelete(nullptr);
    }, "rb", 2048, nullptr, 1, nullptr);
    return r;
}

// -- Route: /doUpload (POST, multipart/form-data) ---------------------------------
// The contract:
//   * destination folder comes from the "dir" QUERY arg
//   * filenames longer than 30 chars are truncated, extension preserved
//   * a "/" is inserted between dir and name only if dir does not already end with one
//   * opens LittleFS with mode "w"
//   * success -> 200 "text/html" body "OK"
//     failure -> 500 "text/plain" body "500"
//   * if dir contains "image", the running Picture app must reload its album, via the
//     _appReloadFn hook.
//
// WHY THIS EXISTS HERE: /doUpload previously lived ONLY on the async server (:8080), so the
// UI's upload button - which posts to :80 like everything else - hit the catch-all and
// 404'd. Handling it here means data/index.html needs no edit.
//
// esp_http_server hands us the RAW body, so the multipart envelope is parsed here. It is a
// streaming parse with a rolling match against the boundary: a boundary that straddles two
// recv() chunks is the classic bug in hand-rolled multipart, and a byte-wise rolling match
// cannot miss one. Matched-but-unconfirmed prefix bytes are flushed only once they are
// ruled out as the delimiter, so file content is never corrupted or truncated.
static constexpr size_t UPLOAD_RECV_SIZE = 1024;
static constexpr size_t UPLOAD_WRITE_SIZE = 512;
static uint8_t s_uploadRecv[UPLOAD_RECV_SIZE];
static uint8_t s_uploadWrite[UPLOAD_WRITE_SIZE];

// Names over 30 chars are truncated, keeping the extension.
static String truncateUploadName(const String& name) {
    if (name.length() <= 30) return name;
    const int dot = name.lastIndexOf('.');
    const String ext = (dot >= 0) ? name.substring(dot) : String(".gif");
    String base = name.substring(0, 20);
    return base + ext;
}

// GET /update - a minimal form so a browser can flash the device. Deliberately tiny: it
// lives in .rodata and its only job is to post a .bin.
//
// The file field is named "firmware", which is the conventional name external OTA tooling
// sends. The POST handler itself does not care - it scans the part header for filename= and
// never inspects the field name - so this is purely about what clients expect.
static esp_err_t h_updatePage(httpd_req_t* req) {
    // Prefer /update.html from the filesystem, so the page can be styled and iterated on like
    // the rest of the web UI. The built-in page below is the FALLBACK, and it has to stay: it
    // lives in .rodata, so it is the only upload form a device with an empty or broken
    // filesystem can still offer - which is how such a device gets re-imaged over WiFi.
    // The existence check takes the FS lock in its own scope and RELEASES it before handing
    // over: sendFsFile takes the lock itself, and holding it across that call would deadlock.
    bool haveFsPage;
    {
        FSUtil::Guard fsg;
        haveFsPage = LittleFS.exists(PATH_UPDATE_HTML);
    }
    if (haveFsPage) return sendFsFile(req, PATH_UPDATE_HTML);
    // The radios sit OUTSIDE the form on purpose: the multipart parser expects the FIRST part
    // to be the file, so a field submitted ahead of it would break the upload. They only pick
    // the action URL. A filesystem write from here always asks to keep the WiFi settings -
    // on a device whose filesystem is already gone there is nothing to keep, and the stash
    // simply finds no SSID.
    // One function sets the form action from the two controls, so the keep-wifi box behaves the
    // same here as on the styled page: greyed out unless Filesystem is chosen, because a
    // firmware write has no /.sys to keep.
    static const char PAGE[] =
        "<html><body style=\"font-family:sans-serif\"><h3>SmolTV PRO OTA</h3>"
        "<p><label><input type=\"radio\" name=\"t\" id=\"ta\" checked onclick=\"u()\">"
        "Firmware</label> "
        "<label><input type=\"radio\" name=\"t\" id=\"tf\" onclick=\"u()\">"
        "Filesystem</label></p>"
        "<p id=\"kr\" style=\"opacity:.45\"><label>"
        "<input type=\"checkbox\" id=\"kw\" checked disabled onclick=\"u()\">"
        " Keep Wi-Fi settings</label></p>"
        "<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">"
        "<input type=\"file\" name=\"firmware\" accept=\".bin\" required><br><br>"
        "<button type=\"submit\">Flash</button></form>"
        "<p style=\"font-size:12px;color:#555\">Firmware: firmware.bin, and it can roll back."
        " Filesystem: littlefs.bin, which replaces every file and cannot.</p>"
        "<script>function u(){var f=document.getElementById('tf').checked,"
        "k=document.getElementById('kw');k.disabled=!f;"
        "document.getElementById('kr').style.opacity=f?1:.45;"
        "document.forms[0].action=f?('/update?target=fs'+(k.checked?'&keepwifi=1':'')):'/update';}"
        "u();</script>"
        "</body></html>";
    return sendBody(req, 200, "text/html", PAGE);
}

static esp_err_t h_doUpload(httpd_req_t* req) {
    // --- boundary from the Content-Type header ---
    char ctype[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK)
        return sendBody(req, 500, "text/plain", "500");
    const char* bp = strstr(ctype, "boundary=");
    if (!bp) return sendBody(req, 500, "text/plain", "500");
    bp += 9;
    if (*bp == '"') bp++;
    String boundary(bp);
    const int qi = boundary.indexOf('"');
    if (qi >= 0) boundary = boundary.substring(0, qi);
    // A part ends with CRLF + "--" + boundary.
    const String delim = String("\r\n--") + boundary;
    const size_t delimLen = delim.length();

    // --- destination directory from the query ---
    const String q = queryOf(req);
    QueryParams P(q.c_str());
    String dir = P.has("dir") ? P.get("dir") : String("/image");

    // --- streaming parse ---
    // Phase 0: part headers (up to the blank line) - carries the filename.
    // Phase 1: file content, terminated by `delim`.
    int    phase   = 0;
    String hdr;                       // bounded below; part headers are small
    String filename;
    File   out;
    size_t written = 0;
    size_t matched = 0;               // rolling boundary-match length
    size_t wlen    = 0;               // bytes pending in s_uploadWrite
    bool   failed  = false;
    int    remaining = req->content_len;

    // OTA MODE. /update posts the same multipart shape as /doUpload, so ONE parser serves
    // both and the only difference is the SINK: Update instead of a File. Bringing OTA onto
    // this server is what allowed the second, asynchronous server to be deleted.
    //
    // pathOf(), NOT req->uri: that carries the query string, so /update?target=fs would have
    // failed this test and been treated as a FILE UPLOAD - writing a firmware image into
    // /image as a file.
    const bool otaMode = (pathOf(req) == ROUTE_UPDATE);

    // Which partition. NO query, or any other value, means the firmware - so a plain
    // POST /update keeps behaving exactly as it always has, which is what the built-in page
    // and any external tooling send. ?target=fs is this firmware's own extension.
    const String otaQuery = queryOf(req);
    QueryParams OQ(otaQuery.c_str());
    const bool otaFs   = otaMode && OQ.has("target") && OQ.get("target") == "fs";
    const bool keepWifi = otaFs && OQ.has("keepwifi") && OQ.get("keepwifi") == "1";
    bool fsUnmounted = false;

    // -- REFUSE THE WRONG IMAGE, BEFORE ANYTHING IS DESTROYED ---------------------------
    //
    // Both images identify themselves, but the updater only checks one of them:
    //
    //   firmware.bin   0xE9 at byte 0        Update DOES check this, for U_FLASH only
    //   littlefs.bin   "littlefs" at byte 8  Update checks NOTHING for U_SPIFFS
    //
    // So posting a firmware image to ?target=fs was the one direction with no guard at all:
    // it would overwrite the filesystem with an app image, and the mount failure on the next
    // boot would then FORMAT the partition. A mistake nobody could take back.
    //
    // The first bytes of the file part are therefore held back and checked before the
    // filesystem is unmounted and before Update.begin() is called. Refusing costs nothing:
    // the device is still fully mounted and serving.
    static constexpr size_t OTA_HEAD_LEN = 16;    // 8 for the offset + 8 for "littlefs"
    uint8_t otaHead[OTA_HEAD_LEN];
    size_t  otaHeadLen = 0;
    bool    otaStarted = false;
    bool    otaWrongImage = false;

    auto flushWrite = [&]() {
        if (wlen) {
            if (otaMode) {
                if (Update.isRunning() && Update.write(s_uploadWrite, wlen) != wlen) {
                    Update.abort(); failed = true;
                }
                written += wlen;
            } else if (out) {
                if (out.write(s_uploadWrite, wlen) != wlen) failed = true;
                written += wlen;
            }
        }
        wlen = 0;
    };
    // Everything the OTA start needs, run once the header has been seen and accepted.
    auto otaStart = [&]() -> bool {
        if (otaFs) {
            if (memcmp(otaHead + 8, "littlefs", 8) != 0) {
                LOGF("[sync] OTA target=fs REFUSED: not a littlefs image (head %02x %02x)",
                     otaHead[0], otaHead[8]);
                return false;
            }
        } else if (otaHead[0] != ESP_IMAGE_HEADER_MAGIC) {
            LOGF("[sync] OTA target=app REFUSED: not an app image (byte0 %02x)", otaHead[0]);
            return false;
        }
        if (otaFs) {
            // Past this line the device cannot read a file until it reboots - which is why
            // the check above happens first.
            if (keepWifi) WiFiMgr::stashCredentials();
            LittleFS.end();
            fsUnmounted = true;
            LOGF("[sync] OTA target=fs: filesystem unmounted%s",
                 keepWifi ? ", wifi stashed" : "");
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, otaFs ? U_SPIFFS : U_FLASH)) {
            LOGF("[sync] OTA begin FAILED: %s", Update.errorString());
            Update.abort();
            return false;
        }
        LOGF("[sync] OTA begin target=%s", otaFs ? "fs" : "app");
        return true;
    };

    auto emit = [&](uint8_t b) {
        // Hold back the header until there is enough of it to judge.
        if (otaMode && !otaStarted) {
            otaHead[otaHeadLen++] = b;
            if (otaHeadLen < OTA_HEAD_LEN) return;
            if (!otaStart()) { otaWrongImage = true; failed = true; return; }
            otaStarted = true;
            for (size_t k = 0; k < otaHeadLen; k++) {
                s_uploadWrite[wlen++] = otaHead[k];
                if (wlen == UPLOAD_WRITE_SIZE) flushWrite();
            }
            return;
        }
        s_uploadWrite[wlen++] = b;
        if (wlen == UPLOAD_WRITE_SIZE) flushWrite();
    };

    while (remaining > 0 && !failed) {
        const int want = (remaining < (int)UPLOAD_RECV_SIZE) ? remaining : (int)UPLOAD_RECV_SIZE;
        const int got  = httpd_req_recv(req, (char*)s_uploadRecv, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { failed = true; break; }
        remaining -= got;

        for (int i = 0; i < got && !failed; i++) {
            const uint8_t b = s_uploadRecv[i];

            if (phase == 0) {
                if (hdr.length() < 512) hdr += (char)b;
                if (hdr.endsWith("\r\n\r\n")) {
                    const int fi = hdr.indexOf("filename=\"");
                    if (fi >= 0) {
                        const int fe = hdr.indexOf('"', fi + 10);
                        if (fe > fi) filename = hdr.substring(fi + 10, fe);
                    }
                    if (filename.isEmpty()) { failed = true; break; }
                    filename = truncateUploadName(filename);

                    if (otaMode) {
                        // NOTHING starts here any more. otaStart() runs from emit(), once the
                        // first OTA_HEAD_LEN bytes have arrived and identified the image - so a
                        // firmware image posted to ?target=fs is refused while the filesystem
                        // is still mounted and the device still healthy.
                        //
                        // Callers should also park the device on an app that holds no files
                        // open (dev.py parks on Clock, and so does the OTA page): the Picture
                        // app keeps a GIF open across frames, and pulling the filesystem out
                        // from under that is not something a lock can save.
                        LOGF("[sync] OTA %d bytes to receive, target=%s",
                             remaining, otaFs ? "fs" : "app");
                        phase = 1;
                        continue;
                    }

                    String path = dir;
                    if (!path.isEmpty() && path != "/" && !LittleFS.exists(path))
                        LittleFS.mkdir(path);
                    if (path.isEmpty() || path == "/") path = "/" + filename;
                    else if (path.endsWith("/"))        path += filename;
                    else                                path += "/" + filename;

                    out = LittleFS.open(path, "w");
                    if (!out) { failed = true; break; }
                    LOGF("[sync] upload -> %s", path.c_str());
                    phase = 1;
                }
                continue;
            }

            // phase 1: content, with a rolling match so a boundary split across two
            // recv() chunks still terminates correctly.
            if (b == (uint8_t)delim[matched]) {
                if (++matched == delimLen) { phase = 2; break; }   // end of file part
                continue;
            }
            if (matched) {
                // Not the delimiter after all - the held-back prefix is real file data.
                // Re-test this byte against delim[0] afterwards (it may start a new match).
                for (size_t k = 0; k < matched; k++) emit((uint8_t)delim[k]);
                matched = 0;
                if (b == (uint8_t)delim[0]) { matched = 1; continue; }
            }
            emit(b);
        }
        if (phase == 2) break;
    }

    flushWrite();
    if (otaMode) {
        const bool otaOk = !failed && phase == 2 && Update.isRunning() && Update.end(true);
        if (!otaOk) {
            if (otaWrongImage) {
                // Refused before anything was touched: the filesystem is still mounted and the
                // running firmware is untouched, so this is a plain 400 and no reboot.
                //
                // DRAIN FIRST. The refusal happens 16 bytes in, but the client is committed to
                // pushing the whole image, and leaving megabytes unread wedges this single-task
                // server until it gives up - measured as ~15 s of "device not answering" after
                // a rejected 3.8 MB post. Reading and discarding costs a few seconds and keeps
                // the server responsive throughout.
                const unsigned long drainStart = millis();
                size_t drained = 0;
                while (remaining > 0 && millis() - drainStart < 30000) {
                    const int want = (remaining < (int)UPLOAD_RECV_SIZE)
                                         ? remaining : (int)UPLOAD_RECV_SIZE;
                    const int got = httpd_req_recv(req, (char*)s_uploadRecv, want);
                    if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
                    if (got <= 0) break;
                    remaining -= got;
                    drained += got;
                }
                LOGF("[sync] OTA refused: drained %u bytes in %lums, %d left",
                     (unsigned)drained, millis() - drainStart, remaining);
                Update.abort();
                return sendBody(req, 400, "text/html",
                    otaFs ? "<html><body><h3>That is not a filesystem image.</h3>"
                            "<p>Expected littlefs.bin. Nothing was written - the device is "
                            "untouched.</p></body></html>"
                          : "<html><body><h3>That is not a firmware image.</h3>"
                            "<p>Expected firmware.bin. Nothing was written - the device is "
                            "untouched.</p></body></html>");
            }
            LOGF("[sync] OTA FAILED target=%s phase=%d written=%u err=%s",
                 otaFs ? "fs" : "app", phase, (unsigned)written, Update.errorString());
            Update.abort();
            if (fsUnmounted) {
                // The filesystem is unmounted and now half-written. Nothing on the device can
                // read a file, so staying up is not an option - reboot and let LittleFS.begin's
                // format-on-failure produce an empty but valid filesystem. The device comes back
                // in AP mode serving the built-in /update page, which is how it gets re-imaged.
                LOGF("[sync] filesystem left unmounted - rebooting to remount or format");
                sendBody(req, 500, "text/html",
                         "<html><body><h3>Filesystem update FAILED. Rebooting - the device "
                         "will come up in AP mode at 192.168.4.1 for another attempt.</h3>"
                         "</body></html>");
                xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(1500)); ESP.restart(); },
                            "ota_rst", 2048, nullptr, 1, nullptr);
                return ESP_OK;
            }
            return sendBody(req, 500, "text/plain", "Update failed");
        }
        LOGF("[sync] OTA OK target=%s %u bytes - rebooting",
             otaFs ? "fs" : "app", (unsigned)written);
        // Answer BEFORE restarting, and restart from a throwaway task, so this handler can
        // return and httpd can flush and close the socket cleanly - same shape as the async
        // handler's 1500 ms delayed ESP.restart().
        sendBody(req, 200, "text/html",
                 otaFs ? "<html><body><h3>Filesystem written. Rebooting...</h3></body></html>"
                       : "<html><body><h3>Update OK. Rebooting...</h3></body></html>");
        xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(1500)); ESP.restart(); },
                    "ota_rst", 2048, nullptr, 1, nullptr);
        return ESP_OK;
    }
    const bool ok = !failed && out && phase == 2;
    if (out) out.close();

    if (!ok) {
        LOGF("[sync] upload FAILED (phase=%d written=%u)", phase, (unsigned)written);
        return sendBody(req, 500, "text/plain", "500");
    }

    // Uploading into /image must flag the running Picture app to reload its album.
    if (dir.indexOf("image") >= 0 && _appReloadFn) _appReloadFn();

    LOGF("[sync] upload OK %s %u bytes", filename.c_str(), (unsigned)written);
    return sendBody(req, 200, "text/html", "OK");
}

// -- Start -----------------------------------------------------------------------
void begin() {
    if (s_server) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.max_open_sockets  = MAX_SOCKETS;
    cfg.backlog_conn      = BACKLOG;
    // When all slots are busy, recycle the least-recently-used session instead of
    // refusing the connection - the browser waits rather than rendering a broken image.
    cfg.lru_purge_enable  = true;
    // Pin to core 0, WITH the WiFi and lwIP TCP/IP tasks (both default to core 0 in
    // arduino-esp32). Two reasons: this task's work is almost entirely talking to the
    // lwIP task, so co-location avoids cross-core switches on every socket call; and it
    // keeps serving OFF core 1, where the Arduino loopTask runs AppCtrl::runLoop and the
    // LVGL pump - so a browser hammering the server cannot starve the display. The httpd
    // default is tskNO_AFFINITY, which lets it float onto core 1 and contend.
    cfg.core_id           = 0;
    // Handlers touch LittleFS and ArduinoJson; the 4096 default is tight.
    cfg.stack_size        = 8192;
    // Do not outrank WiFi/lwIP (which run higher) - httpd's default of +5 is correct.
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    // 11 routes are registered below; httpd calloc's this many slots once at start, so the
    // headroom is a few hundred bytes and a silent "register failed" is far worse.
    cfg.max_uri_handlers  = 20;   // 13 registered after OTA
    // Enables "/*" so one handler can serve every unmatched path.
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    const esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        LOGF("[sync] httpd_start FAILED: %s", esp_err_to_name(err));
        s_server = nullptr;
        return;
    }

    auto regM = [](const char* uri, esp_err_t (*fn)(httpd_req_t*), httpd_method_t m) {
        httpd_uri_t u = {};
        u.uri = uri; u.method = m; u.handler = fn; u.user_ctx = nullptr;
        const esp_err_t e = httpd_register_uri_handler(s_server, &u);
        if (e != ESP_OK) LOGF("[sync] register %s failed: %s", uri, esp_err_to_name(e));
    };
    auto reg = [&](const char* uri, esp_err_t (*fn)(httpd_req_t*)) {
        regM(uri, fn, HTTP_GET);
    };

    // Specific routes first; the wildcard catch-all must be registered LAST so it does
    // not shadow them (httpd matches in registration order).
    reg(ROUTE_SET,        h_set);
    // The UI posts uploads to :80; /set is also accepted as POST because the real UI uses
    // both verbs against it.
    regM("/set",       h_set,      HTTP_POST);
    regM(ROUTE_UPLOAD, h_doUpload, HTTP_POST);
    // OTA on :80 - the prerequisite for deleting the second server.
    // Same handler as /doUpload; h_doUpload branches on the URI into its Update sink.
    reg(ROUTE_UPDATE,     h_updatePage);
    regM("/update",    h_doUpload, HTTP_POST);
    reg(ROUTE_WIFI_JSON,  h_wifiJson);
    reg(ROUTE_WIFISAVE,   h_wifiSave);
    regM("/wifisave",  h_wifiSave, HTTP_POST);
    reg(ROUTE_SPACE,      h_space);
    reg(ROUTE_V_JSON,     h_vjson);
    reg(ROUTE_DELETE,     h_delete);
    reg(ROUTE_FILELIST,   h_filelist);
    reg("/*",          h_static);

    LOGF("[sync] httpd up on :80 core=%d sockets=%u backlog=%u buf=%u free=%u blk8=%u",
         (int)cfg.core_id, (unsigned)cfg.max_open_sockets, (unsigned)cfg.backlog_conn,
         (unsigned)STREAM_BUF_SIZE, (unsigned)ESP.getFreeHeap(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

} // namespace SyncWeb
