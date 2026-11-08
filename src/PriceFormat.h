#pragma once
#include <Arduino.h>

// -- Shared crypto price formatter -----------------------------------------------
// The Coin and CoinGecko renderers share this, so their formatting is identical.
//
// Decimal places are chosen by magnitude:
//     v <  10.0             6 decimals
//     10.0 <= v <= 999.0    2 decimals
//     v >  999.0            integer only from 10000.0 up
//
// then:
//   1. integer part -> base-10, thousands separator every 3 digits from the right;
//   2. if v >= 10000.0 stop there - integer only;
//   3. else dtostrf(v, 0, decimals), take the fraction, STRIP TRAILING ZEROS, and
//      append decSep + fraction only if anything survives.
// Step 3 is why a price renders as "211.5" and not "211.50".
//
// Separators come from sep.json. The initial '.' / ',' below are the defaults used when
// that file is absent, which is why an unconfigured device still renders "64,479".
//
// THESE ARE GLOBALS, and that is the fix for a save that went nowhere. sep.json was being
// written by the web UI (via "Save Market Style", not "Save Bitcoin" - the separators ride
// with the palette) and read back into AppCoin::_decSep/_thouSep, but the only consumer of
// those members was AppCoin::_formatPrice, which NOTHING calls: every actual render site
// (LvCoin update/updateLarge/_fillRow and the three matching LvCoinGecko ones - six in all)
// invoked formatCoinPrice(price) with no separator arguments, so it silently used the
// defaults. The setting round-tripped through the filesystem and never reached a pixel.
// Making the defaults refer to these globals fixes all six call sites at once; the apps'
// reloadConfig assigns them. sep.json is shared by Coin and CoinGecko, so one pair is right.
inline String g_decSep  = ".";
inline String g_thouSep = ",";

inline String formatCoinPrice(float v, const String& decSep = g_decSep,
                              const String& thouSep = g_thouSep) {
    int decimals;
    bool intOnly = false;
    if (v < 10.0f)         decimals = 6;
    else if (v <= 999.0f)  decimals = 2;
    else if (v >= 3000.0f) { decimals = 0; intOnly = true; }
    else                   decimals = 2;

    char ibuf[24];
    snprintf(ibuf, sizeof(ibuf), "%ld", (long)v);
    String ip(ibuf);
    for (int i = (int)ip.length() - 3; i > 0; i -= 3)
        ip = ip.substring(0, i) + thouSep + ip.substring(i);
    if (intOnly || v >= 10000.0f) return ip;

    char fbuf[32];
    dtostrf(v, 0, decimals, fbuf);
    String f(fbuf);
    const int dot = f.indexOf('.');
    if (dot < 0) return ip;
    String frac = f.substring(dot + 1);
    while (frac.length() && frac.charAt(frac.length() - 1) == '0')
        frac.remove(frac.length() - 1);
    if (frac.isEmpty()) return ip;
    return ip + decSep + frac;
}
