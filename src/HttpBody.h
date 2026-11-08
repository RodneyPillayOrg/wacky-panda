#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include "Log.h"

// -- Reading a chunked HTTP body reliably ----------------------------------------
// Two traps, both hit while getting the CoinGecko chart to render:
//
//  1. HTTPClient::getStream() returns the RAW socket. When the server replies with
//     "Transfer-Encoding: chunked" and no Content-Length - which api.coingecko.com
//     does - the chunk-size lines are still in the byte stream. Handing that to
//     ArduinoJson makes it parse the leading hex chunk length as a bare number,
//     report success, and produce a document with no members.
//
//  2. HTTPClient::getString() de-chunks correctly but discards the transfer error,
//     so a short read looks like a short body. It also grows a StreamString by
//     repeated reserve(), which on a fragmented ~40 KB-max-block heap is exactly
//     where it fails.
//
// writeToStream() does the de-chunking AND returns the byte count or a negative
// HTTPC_ERROR_*, so this appends into a String reserved once up front and reports
// what actually happened.
// One shared scratch buffer for HTTP response bodies. AppCtrl runs exactly one app's
// loop() on one task, and every fetch inside an app is sequential, so a single buffer
// is safe. Sharing it (rather than a static per call site) keeps .bss down, which
// matters directly: mbedTLS needs a large contiguous heap block for its handshake,
// and every kilobyte of .bss is a kilobyte the heap does not get.
// 4608, not 8192. EXPERIMENT TURNED FIX: in one build the Yahoo handshake failed at
// blk8 = 38900 while the CoinGecko handshake SUCCEEDED at blk8 = 42996 - and Yahoo's
// certificate chain is 6333 PEM bytes against CoinGecko's 3532. So Yahoo needs MORE
// contiguous heap and had ~4 KB LESS of it. mbedTLS holds two content buffers plus the
// parsed chain during a handshake, which puts this right on the boundary; shrinking
// this buffer hands the margin back. 4608 still clears the largest body actually
// measured (a 3259-byte CoinGecko chart response).
// 6144, was 4608. The 4608 was chosen to hand blk8 margin to the mbedTLS handshake back
// when TLS was on the edge. TLS is now solved (framework match -> ~38 KB contiguous blk8,
// well over the 33434 the handshake needs), and Binance's 30-kline response for the Coin
// app is ~5 KB - it was truncating at 4607 and failing to parse ("JSON IncompleteInput"),
// leaving app_bitcoin stuck on "Loading...". 6144 clears it with margin. Still .bss (the
// app path stays allocation-free); the extra 1.5 KB is well within the TLS blk8 headroom.
inline char HTTP_BODY[6144];
constexpr size_t HTTP_BODY_CAP = sizeof(HTTP_BODY);

// Appends into a caller-supplied fixed buffer. NO heap at all.
//
// This replaced a String-based sink that could not be made to work: writeToStream()
// reported the full 3259 bytes transferred while the String had retained only 1890 of
// them, because String::concat's grow-by-realloc kept failing even with a 40 KB
// largest free block reported. A fixed buffer keeps the app path allocation-free, for the
// same reason the display uses a static band buffer rather than a heap framebuffer.
class BufferSink : public Stream {
public:
    BufferSink(char* buf, size_t cap) : _buf(buf), _cap(cap) {}
    size_t write(uint8_t c) override {
        if (_len + 1 >= _cap) { _overflow = true; return 0; }
        _buf[_len++] = (char)c;
        _buf[_len] = 0;
        return 1;
    }
    size_t write(const uint8_t* b, size_t n) override {
        if (_len + n >= _cap) { _overflow = true; n = (_cap > _len + 1) ? _cap - _len - 1 : 0; }
        memcpy(_buf + _len, b, n);
        _len += n;
        _buf[_len] = 0;
        return n;
    }
    int  available() override { return 0; }
    int  read() override      { return -1; }
    int  peek() override      { return -1; }
    void flush() override     {}
    size_t length() const     { return _len; }
    bool   overflowed() const { return _overflow; }
private:
    char*  _buf;
    size_t _cap;
    size_t _len = 0;
    bool   _overflow = false;
};

// Reads the (possibly chunked) body into `buf`, always NUL-terminated. Returns the
// writeToStream() result: >= 0 is the byte count sent, negative is an HTTPC_ERROR_*.
// `outLen` receives what was actually stored, which is less than the return value if
// the buffer was too small.
inline int httpBodyToBuffer(HTTPClient& http, char* buf, size_t cap, size_t& outLen) {
    buf[0] = 0;
    outLen = 0;
    BufferSink sink(buf, cap);
    const int rc = http.writeToStream(&sink);
    outLen = sink.length();
    if (sink.overflowed())
        LOGF("[http] body truncated at %u of %d bytes", (unsigned)outLen, rc);
    else if (rc < 0)
        LOGF("[http] writeToStream rc=%d got=%u", rc, (unsigned)outLen);
    return rc;
}

class StringSink : public Stream {
public:
    explicit StringSink(String& out) : _out(out) {}
    size_t write(uint8_t c) override { _out += (char)c; return 1; }
    size_t write(const uint8_t* b, size_t n) override {
        _out.concat((const char*)b, n);
        return n;
    }
    int  available() override { return 0; }
    int  read() override      { return -1; }
    int  peek() override      { return -1; }
    void flush() override     {}
private:
    String& _out;
};

// Returns the writeToStream() result: >= 0 is the byte count, negative is an
// HTTPC_ERROR_* code (-11 == READ_TIMEOUT, -4 == CONNECTION_LOST).
inline int httpBodyToString(HTTPClient& http, String& out, size_t reserveBytes) {
    out = "";
    // The up-front reserve is an OPTIMISATION, not a requirement - it avoids the
    // realloc churn that makes StreamString fail on a fragmented heap. It is not
    // fatal when it fails: String::concat still grows the buffer on demand, and
    // bailing out here turned a working transfer into an empty body.
    if (!out.reserve(reserveBytes)) {
        LOGF("[http] reserve(%u) failed, growing on demand (maxblk=%u)",
             (unsigned)reserveBytes, (unsigned)ESP.getMaxAllocHeap());
    }
    StringSink sink(out);
    const int rc = http.writeToStream(&sink);
    if (rc < 0) LOGF("[http] writeToStream rc=%d got=%u maxblk=%u", rc,
                     (unsigned)out.length(), (unsigned)ESP.getMaxAllocHeap());
    return rc;
}
