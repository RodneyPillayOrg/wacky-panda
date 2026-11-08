#pragma once

// -- SyncWeb - the bomb-proof HTTP server on port 80 ------------------------------
// DECLARATION ONLY, deliberately. The implementation (SyncWeb.cpp) includes
// esp_http_server.h, which pulls in nghttp's http_parser.h and declares HTTP_GET /
// HTTP_POST / ... as a sequential http_method enum. ESPAsyncWebServer declares the same
// names as an incompatible BITMASK enum (WebRequestMethod). Both in one translation unit
// is a hard compile error, and main.cpp needs to start both servers - so the sync
// server's headers stop at the .cpp boundary and only this signature crosses it.
//
// Rationale for the whole design (why the async server fragmented the heap, the lwIP
// no-memory-pools finding, and the measurements): see the header comment in SyncWeb.cpp.
namespace SyncWeb {

// Start the synchronous server on :80. Safe to call more than once (a second call is a
// no-op). Serves the entire web UI: page, images, and the GET config API.
void begin();

} // namespace SyncWeb
