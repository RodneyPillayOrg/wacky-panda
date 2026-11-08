#pragma once
#include <Arduino.h>
#include <vector>
#include <utility>
// Deliberately server-agnostic. It carries no dependency on any particular HTTP server
// so that the /set contract can be shared, and so a second server could be added without
// its HTTP_GET/HTTP_POST enum colliding with esp_http_server's.

// -- Server-agnostic request parameters + response --------------------------------
// The /set dispatcher (handleSetCore in WebRoutes.h) holds the config contract for every
// app - ~320 lines that must not be duplicated per server. It is served by the synchronous
// esp_http_server on :80 (SyncWeb.h), which adapts to this tiny interface; a second server
// would do the same rather than forking that logic.
//
// There WAS a second one, ESPAsyncWebServer on :8080, carrying OTA and upload. It is gone.
// AsyncAbstractResponse::_ack() mallocs a TCP-window-sized buffer
// (CONFIG_LWIP_TCP_SND_BUF_DEFAULT = 5744) per ack per connection; six parallel browser
// connections shredded the 8-bit heap's largest contiguous block from ~70 KB to ~4 KB while
// total free stayed ~60 KB - fragmentation, not exhaustion - which locked the device and
// starved the Picture app's 38 KB band.

// Percent-decode [b,e), turning '+' into space (form-urlencoded semantics). Needed for
// both query values (colours arrive as %23ff0000, units as %C2%B0C) and request paths
// (the UI's mediaUrl() encodeURIComponent()s each path segment, so a filename with a
// space arrives as %20). A malformed escape is passed through verbatim rather than eaten.
inline String httpUrlDecode(const char* b, const char* e) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    String out;
    out.reserve(e - b);
    while (b < e) {
        const char c = *b;
        if (c == '+') { out += ' '; ++b; }
        else if (c == '%' && (e - b) >= 3) {
            const int hi = hex(b[1]), lo = hex(b[2]);
            if (hi >= 0 && lo >= 0) { out += (char)((hi << 4) | lo); b += 3; }
            else { out += c; ++b; }
        } else { out += c; ++b; }
    }
    return out;
}
inline String httpUrlDecode(const char* s) { return httpUrlDecode(s, s + strlen(s)); }

// Read-only view of a request's query/form parameters.
struct ReqParams {
    virtual ~ReqParams() = default;
    virtual bool   has(const char* key) const = 0;
    virtual String get(const char* key) const = 0;
};

// What a route handler produced, independent of how it gets written to the wire.
// Explicit constructor (rather than default member initializers) so brace-init works
// uniformly across compilers - a String member plus NSDMIs makes the aggregate rules
// version-dependent, and this is constructed at ~8 sites.
struct SetResult {
    int         status;
    const char* contentType;
    String      body;
    SetResult(int s = 200, const char* ct = "text/plain", const String& b = "OK")
        : status(s), contentType(ct), body(b) {}
};

// -- Adapter: raw query string (esp_http_server) ----------------------------------
// Parses "a=1&b=hello%20world&flag" once at construction. Percent-decodes and turns
// '+' into space, matching form-urlencoded semantics - the web UI sends colour values
// like %23ff0000 (#ff0000) and unit strings like %C2%B0C ( deg C), so decoding is required,
// not cosmetic. A valueless key ("flag") is present with an empty value, so has() is
// true for it - same as the async server's hasArg().
class QueryParams : public ReqParams {
public:
    explicit QueryParams(const char* query) {
        if (!query) return;
        const char* p = query;
        while (*p) {
            const char* amp = strchr(p, '&');
            const char* end = amp ? amp : (p + strlen(p));
            const char* eq  = nullptr;
            for (const char* s = p; s < end; ++s) if (*s == '=') { eq = s; break; }
            if (eq) _kv.push_back({ _decode(p, eq), _decode(eq + 1, end) });
            else if (end > p) _kv.push_back({ _decode(p, end), String() });
            if (!amp) break;
            p = amp + 1;
        }
    }
    bool has(const char* key) const override {
        for (const auto& e : _kv) if (e.first == key) return true;
        return false;
    }
    String get(const char* key) const override {
        for (const auto& e : _kv) if (e.first == key) return e.second;
        return String();
    }
private:
    std::vector<std::pair<String, String>> _kv;
    static String _decode(const char* b, const char* e) { return httpUrlDecode(b, e); }
};
