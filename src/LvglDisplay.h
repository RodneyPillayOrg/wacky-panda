// NOTE THE FILENAME: this MUST NOT be called Lvgl.h. Windows filesystems are
// case-insensitive, so with src/ on the include path `#include <lvgl.h>` below
// resolves to THIS FILE instead of the library header. `#pragma once` then makes it
// a no-op, and every LVGL type silently fails to declare -- with lvgl.h reported as
// found, so the error looks like a broken lv_conf.h rather than a name collision.
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

// -- LVGL bring-up -----------------------------------------------------------------------
//
// Boot order: ledc backlight channel, setRotation(0), TFT init, DISPON, initDMA at 60 MHz,
// optional splash, then lv_init, lv_disp_draw_buf_init, lv_disp_drv_init with 240x240 and
// flushCb below, lv_disp_drv_register.
//
// THE DRAW BUFFER IS STATIC AND SINGLE: 19200 pixels = 240 x 80, in .bss (38400 bytes). An
// earlier renderer malloc'd a 38400-byte TFT_eSprite via createSprite() instead, punching a
// hole in the heap. Moving it to .bss does NOT free memory - .bss and the heap share DRAM on
// ESP32 - but it keeps the heap CONTIGUOUS, which matters because mbedTLS needs ~32 KB of
// internal DRAM per TLS session on a board with no PSRAM.
namespace Lv {

static constexpr int16_t H_RES  = 240;   // drv resolution word 0xf000f0
static constexpr int16_t V_RES  = 240;
static constexpr int16_t BAND_H = 80;    // 0x4b00 px / 240 == 80 rows

// The draw buffer. Static, single, never heap-allocated - see above.
inline lv_color_t          drawBuf[H_RES * BAND_H];
inline lv_disp_draw_buf_t  dispBuf;
inline lv_disp_drv_t       dispDrv;
inline TFT_eSPI*           panel = nullptr;

// lvgl_disp_flush_cb, transcribed:
//     w = x2 - x1 + 1;  h = y2 - y1 + 1
//     setWindow(x1, y1, w, h)
//     rows = 0x10000 / w                       // chunk height, in LINES
//     while (h) {
//         n = min(rows, h);  h -= n;
//         setSwapBytes(true);                  // helper(tft, 1)
//         pushImageDMA(x1, y, w, n, p, nullptr);
//         dmaWait();
//         p += w * n;  y += n;
//     }
//     lv_disp_flush_ready(drv)
//
// The 0x10000/w chunking is a byte-count cap on a single DMA transfer, so for w = 240 it is
// 273 rows - more than any band - and the loop runs exactly once in practice. It is kept
// because it matters for any narrower area LVGL decides to flush.
inline void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* colorP) {
    const uint32_t w = (area->x2 - area->x1) + 1;
    uint32_t       h = (area->y2 - area->y1) + 1;

    panel->setWindow(area->x1, area->y1, w, h);

    const uint32_t rows = 0x10000UL / w;
    int32_t        y    = area->y1;
    uint16_t*      p    = reinterpret_cast<uint16_t*>(colorP);

    while (h) {
        const uint32_t n = (rows < h) ? rows : h;
        h -= n;
        // setSwapBytes(true) before EVERY push. This is also why LV_COLOR_16_SWAP is 0 in
        // lv_conf.h - swapping in both places would invert the channels.
        panel->setSwapBytes(true);
        // PIO, not pushImageDMA. TFT_eSPI's DMA setup breaks its own write path on
        // Arduino core 3 / ESP-IDF 5 (see the note in Display::begin), and this loop
        // always waited out each transfer before reusing the band anyway, so DMA bought
        // no overlap here. Same bytes, same order, same wait - minus the broken setup.
        panel->pushImage(area->x1, y, (int32_t)w, (int32_t)n, p);
        p += w * n;
        y += n;
    }
    lv_disp_flush_ready(drv);
}

// The lv_init + draw-buffer + driver sequence from display_boot_task. `tft` must already
// be initialised (init + setRotation + DISPON + initDMA), matching the order there:
// the panel is up and the splash is done BEFORE lv_init() is called.
inline void begin(TFT_eSPI& tft) {
    panel = &tft;
    lv_init();
    lv_disp_draw_buf_init(&dispBuf, drawBuf, nullptr, H_RES * BAND_H);
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res  = H_RES;
    dispDrv.ver_res  = V_RES;
    dispDrv.draw_buf = &dispBuf;
    dispDrv.flush_cb = flushCb;
    lv_disp_drv_register(&dispDrv);
}

// LV_TICK_CUSTOM is 1 (LVGL reads millis() itself), so only the handler needs pumping.
// It must also be pumped inside the blocking transition wait,
// `while (lv_anim_count_running()) lv_timer_handler();`, in AppCtrl's screen transition.
inline uint32_t tick() { return lv_timer_handler(); }

// -- RGB565 colour literals --------------------------------------------------------
// Every LVGL colour argument is an lv_color_t passed BY VALUE, and at
// LV_COLOR_DEPTH 16 that struct is one 16-bit word - so a literal like 0xF061 on an
// lv_obj_set_style_*_color call is RGB565, NOT 24-bit RGB.
//
// lv_color_hex() takes RGB888, so feeding it these literals silently reinterprets them:
// 0xF061 became 0x00F061, a GREEN, where it should be bright red (248,12,8); and the
// weekday's 0x07BF became a dark blue instead of cyan. Use this for any 16-bit colour word,
// and lv_color_hex() only for genuine RGB888 (e.g. the "#87cefa" recolor-markup strings).
inline lv_color_t c565(uint16_t v) {
    lv_color_t c;
    c.full = v;
    return c;
}

// -- One screen alive at a time ---------------------------------------------------
// Each screen destroys the previous one before building: an app's onEnter tears down what
// was there, and the launcher does the same. Keeping the boot screen, the launcher and an
// app screen all alive simultaneously is not the design.
//
// It is NOT here to fix a memory problem. It was introduced on the theory that three live
// screens had exhausted LV_MEM_SIZE, and lv_mem_monitor then disproved that outright: with
// all three alive the pool sat at 40% used / 9,912 B free / 1% frag. The blank Clock screen
// had a different cause entirely (nothing was pumping lv_timer_handler once an app owned
// the panel - see arduino_loop). Do not reintroduce the memory story.
//
// `slot` is the owning module's own screen pointer. It is NULLED when its screen is
// destroyed, so the module knows to rebuild (and can reset its child pointers, which die
// with the parent).
inline lv_obj_t*  curScreen = nullptr;
inline lv_obj_t** curSlot   = nullptr;

inline void adoptScreen(lv_obj_t* s, lv_obj_t** slot) {
    lv_obj_t*  oldScreen = curScreen;
    lv_obj_t** oldSlot   = curSlot;
    curScreen = s;
    curSlot   = slot;
    lv_scr_load(s);                       // load BEFORE deleting the outgoing one
    if (oldScreen && oldScreen != s) {
        // `oldSlot != slot` matters: when a module replaces its OWN screen (e.g. LvCoin
        // rebuilding for a new c_theme, where every build* passes &LvCoin::screen), the
        // outgoing slot is the SAME VARIABLE the caller has just pointed at the new screen.
        // Nulling it unconditionally wiped the brand-new pointer, and since every update*
        // guards on `if (!screen) return;` the app then drew nothing at all - the blank
        // light-blue panel seen after switching BitCoin to "1-Coin Large".
        if (oldSlot && oldSlot != slot) *oldSlot = nullptr;
        lv_obj_del(oldScreen);
    }
}

// Pool headroom as a NUMBER, not a guess. Call after building a screen.
inline void logMem(const char* who) {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    LOGF("[lv] %s pool used=%u%% free=%u biggest=%u frag=%u%%", who,
         (unsigned)m.used_pct, (unsigned)m.free_size,
         (unsigned)m.free_biggest_size, (unsigned)m.frag_pct);
}

} // namespace Lv
