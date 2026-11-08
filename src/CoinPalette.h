#pragma once
#include <Arduino.h>
#include "Config.h"          // FSUtil + PATH_CCOLOR live here

// -- Shared crypto price palette (ccolor.json) ------------------------------------
// The BitCoin (Coin) and CoinGecko apps share ONE price palette, exposed in the web UI as
// "Market Style / Shared price palette (Shared with CoinGecko)" and stored in
// /.sys/ccolor.json as #RRGGBB strings.
//
// TEN colours, not four. The first four dress the price panel, selected by the sign of the
// change; the last six were added later and are just as configurable:
//
//     key           default    used for
//     up_fg         #000000    price text, change >= 0
//     up_bg         #00FF00    price-panel background, change >= 0
//     down_fg       #FFFFFF    price text, change < 0
//     down_bg       #FF0000    price-panel background, change < 0
//     coin_fg       #FFFFFF    coin name/symbol text
//     coin_bg       #000000    coin name/symbol background
//     pct_up_fg     #000000    percentage TEXT, change >= 0
//     pct_up_bg     #00FF00    percentage BACKGROUND, change >= 0
//     pct_down_fg   #FFFFFF    percentage TEXT, change < 0
//     pct_down_bg   #FF0000    percentage BACKGROUND, change < 0
//
// The percentage is a BADGE - its own background object plus a label on top - not coloured
// text sitting on the price panel. That distinction has caused two bugs worth naming, both
// from reading a default as if it were a constant:
//   * the percentage colours were once believed fixed green/red and hardcoded as such, so
//     changing them in the web UI did nothing;
//   * pct_up_fg defaults to BLACK, which is invisible until its GREEN background partner is
//     drawn. An invisible up-percentage was once "fixed" by forcing the TEXT green - wrong
//     colour, and applied to the wrong element.
// Both renderers now read all ten; see LvCoin::renderSingle and LvCoinGecko::renderSingle.
namespace CoinPalette {

inline uint16_t up_fg   = 0x0000;   // black
inline uint16_t up_bg   = 0x07E0;   // green
inline uint16_t down_fg = 0xFFFF;   // white
inline uint16_t down_bg = 0xF800;   // red

// The six added later.
inline uint16_t coin_fg     = 0xFFFF;   // white
inline uint16_t coin_bg     = 0x0000;   // black
inline uint16_t pct_up_fg   = 0x0000;   // black  (text of the "up" badge)
inline uint16_t pct_up_bg   = 0x07E0;   // green  (background of the "up" badge)
inline uint16_t pct_down_fg = 0xFFFF;   // white
inline uint16_t pct_down_bg = 0xF800;   // red

// "#RRGGBB" (or "RRGGBB") -> RGB565; falls back to def on a malformed value.
inline uint16_t hex565(const String& h, uint16_t def) {
    String s = h;
    if (s.startsWith("#")) s = s.substring(1);
    if (s.length() < 6) return def;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 16);
    if (end == s.c_str()) return def;
    const uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Reload from ccolor.json (called from both apps' reloadConfig). Absent file -> defaults.
inline void load() {
    up_fg = 0x0000; up_bg = 0x07E0; down_fg = 0xFFFF; down_bg = 0xF800;
    coin_fg = 0xFFFF; coin_bg = 0x0000;
    pct_up_fg = 0x0000; pct_up_bg = 0x07E0;
    pct_down_fg = 0xFFFF; pct_down_bg = 0xF800;
    // 512 not 256: ten "#RRGGBB" keys no longer fit a 256-byte document, and an overflow
    // would silently yield defaults for the later keys.
    DynamicJsonDocument doc(512);
    if (FSUtil::readJson(PATH_CCOLOR, doc)) {
        up_fg       = hex565(doc["up_fg"]       | "#000000", 0x0000);
        up_bg       = hex565(doc["up_bg"]       | "#00ff00", 0x07E0);
        down_fg     = hex565(doc["down_fg"]     | "#ffffff", 0xFFFF);
        down_bg     = hex565(doc["down_bg"]     | "#ff0000", 0xF800);
        coin_fg     = hex565(doc["coin_fg"]     | "#FFFFFF", 0xFFFF);
        coin_bg     = hex565(doc["coin_bg"]     | "#000000", 0x0000);
        pct_up_fg   = hex565(doc["pct_up_fg"]   | "#000000", 0x0000);
        pct_up_bg   = hex565(doc["pct_up_bg"]   | "#00FF00", 0x07E0);
        pct_down_fg = hex565(doc["pct_down_fg"] | "#FFFFFF", 0xFFFF);
        pct_down_bg = hex565(doc["pct_down_bg"] | "#FF0000", 0xF800);
    }
}

} // namespace CoinPalette
