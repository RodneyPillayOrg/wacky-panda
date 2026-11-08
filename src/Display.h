#pragma once
#include <TFT_eSPI.h>
#include "Config.h"
#include "LvglDisplay.h"    // Lv::drawBuf - the ONE band buffer, shared (see BandSprite)

// -- BandSprite - a TFT_eSprite over an externally-owned, static pixel buffer -----
// THE POINT: we were carrying TWO 38,400-byte band buffers - LVGL's static Lv::drawBuf
// in .bss, plus a second one createSprite()'d from the HEAP for the direct-draw apps
// (Picture, Idea). There should be exactly ONE - 19,200 px = 38,400 bytes, passed straight
// to lv_disp_draw_buf_init, see LvglDisplay.h. On a 320 KB-DRAM part the duplicate is
// unaffordable: with both live, the Picture app left
// only ~12 KB contiguous, web serving then starved the WiFi driver of its on-demand RX/TX
// buffers, and the device fell OFF THE NETWORK while its app loop kept running. Making the
// heap copy permanent instead broke TLS (blk8 ~14 KB against mbedTLS's ~34 KB need,
// measured: every crypto fetch returned HTTP -1).
//
// So: adopt LVGL's buffer rather than allocating a second one. Safe because the two owners
// are mutually exclusive by construction - AppCtrl suspends the LVGL pump while a
// direct-draw app is active (`if (!directDraw) Lv::tick()`), and those apps never run
// concurrently with LVGL ones. One buffer, one owner.
//
// createSprite() is bypassed (it calloc()s); adoptStatic() reproduces its 16-bpp,
// single-frame initialisation exactly, against a caller-supplied pointer.
class BandSprite : public TFT_eSprite {
public:
    explicit BandSprite(TFT_eSPI* tft) : TFT_eSprite(tft) {}

    // Mirror of TFT_eSprite::createSprite() for bpp=16/frames=1, minus the allocation.
    void adoptStatic(uint16_t* buf, int16_t w, int16_t h) {
        if (_created || !buf || w < 1 || h < 1) return;
        _bpp      = 16;
        _iwidth   = _dwidth = _bitwidth = w;
        _iheight  = _dheight = h;
        cursor_x  = 0;
        cursor_y  = 0;
        _sx = 0; _sy = 0; _sw = w; _sh = h; _scolor = TFT_BLACK;
        _img8 = _img8_1 = _img8_2 = (uint8_t*)buf;
        _img  = buf;
        _img4 = (uint8_t*)buf;
        _created  = true;
        rotation  = 0;
        setViewport(0, 0, _dwidth, _dheight);
        setPivot(_iwidth / 2, _iheight / 2);
    }

    // The base class would free(_img8_1). Our buffer is .bss, so that would corrupt the
    // heap. Nothing should call this (Display::freeBand is a no-op) - this is the guard
    // rail in case something ever does.
    void deleteSprite(void) { /* never free a static buffer */ }
};

// Rendering architecture (ESP32-D0WD-V3, 8 MB flash, NO PSRAM).
//
// The panel is driven through LVGL + TFT_eSPI SPI-DMA from a single 240x80 band buffer, so
// the 240x240 screen is painted in 3 DMA-flushed bands at zero heap cost. The direct-draw
// path uses one band sprite allocated ONCE in begin() before WiFi (measured: no
// fragmentation, max contiguous stays 110,580) and never freed.
//
// A big full-screen heap framebuffer is NOT viable here: the WiFi driver allocates
// RX buffers from the heap on demand and silently drops frames below ~60KB free,
// which broke ping and large HTTP responses. Banding removes that conflict.
//
// begin()      - hardware init (TFT + backlight + DMA) and band allocation.
// renderApp(cb[,bg]) / renderFrame - full-screen banded draw; cb uses ABSOLUTE
//                240x240 coords, must be PURE, and must NOT call fillSprite().
// showStatus() - the boot screen (title / animated ring / status text).
// status(msg)  - one-line message straight to the panel (fallback if no band).
// tft          - draw direct to the panel; used for image decoding (JPEG/GIF),
//                which is stateful and must not be re-run per band.

class Display {
public:
    static TFT_eSPI    tft;

    // Height of the banded render buffer, in pixels: 240 / 80 = 3 bands, the same geometry
    // as the LVGL draw buffer. This is the ONLY large display buffer, so the heap stays free
    // for WiFi/TLS. Affordable now that the two idle AnimatedGIF decoders (~47 KB static)
    // were replaced by one shared on-demand instance - see GifDecoder.h.
    static constexpr int16_t BAND_H = 80;
    // Set to true ONLY to temporarily resurrect the pre-LVGL renderer for
    // comparison. It will starve LVGL and WiFi of DRAM - see ensureBand().
    static constexpr bool ALLOW_LEGACY_SPRITE = false;
    // Runtime gate: the two GuiLite-style full-screen apps (Idea, Picture) draw via the
    // banded TFT path. AppCtrl sets this true on their onEnter (band allocated) and false
    // on exit (band freed so the crypto apps get their contiguous heap back). Allocation
    // fails safe: if the ~38 KB band can't be had, renderApp() is a no-op (blank), never a
    // crash. While it is true, AppCtrl also stops pumping lv_timer_handler.
    static inline bool _appBandActive = false;
    static void enableAppBand(bool on) {
        _appBandActive = on;
        if (on) {
            // Clear the panel to black as the direct-draw app takes over. The LVGL pump is
            // suspended for these apps, so without this the launcher's last flush (its icon +
            // label) would linger if the band failed to allocate - which reads as the app
            // being "unimplemented / stuck on the transition". Black is the correct empty
            // state; the app's first frame then draws over it (or it stays black on OOM).
            tft.fillScreen(TFT_BLACK);
            ensureBand();
        } else {
            freeBand();
        }
    }
    // Absolute Y range of the band currently being composed (see renderFrame).
    static inline int16_t _bandY0 = 0, _bandY1 = 240;
    static int16_t bandTop()    { return _bandY0; }
    static int16_t bandBottom() { return _bandY1; }

    // -- Boot/status screen ------------------------------------------------------------
    // The background is BLACK. It was once a dark navy 0x0084, which is where the "bg colour
    // is different" report came from.
    static constexpr uint16_t BOOT_BG = 0x0000;
    // Title label: align TOP_MID(2), y=0x12=18, text "SmolTV PRO", colour 0x86FF.
    static constexpr int      BOOT_TITLE_Y  = 18;
    static constexpr uint16_t BOOT_TITLE_C  = 0x86FF;
    // Spinner: lv_spinner_create(parent, 1000, 0x50) => 1000 ms per revolution,
    // 80-degree arc. Size 0x58x0x58 = 88x88, align TOP_MID, y=0x36=54, arc width 8,
    // track colour 0x19C9, indicator colour 0x6F7F.
    static constexpr int      SPIN_Y        = 54;
    static constexpr int      SPIN_SIZE     = 88;
    static constexpr int      SPIN_W        = 8;
    static constexpr uint16_t SPIN_TRACK_C  = 0x19C9;
    static constexpr uint16_t SPIN_IND_C    = 0x6F7F;
    static constexpr uint32_t SPIN_PERIOD_MS = 1000;
    static constexpr int      SPIN_ARC_DEG  = 80;
    // Two status lines: y=0xa8=168 colour 0xF7DF, and y=0xc6=198 colour 0x7F9F.
    static constexpr int      BOOT_L1_Y = 168;
    static constexpr uint16_t BOOT_L1_C = 0xF7DF;
    static constexpr int      BOOT_L2_Y = 198;
    static constexpr uint16_t BOOT_L2_C = 0x7F9F;

    // Hardware init only - no sprite allocation.
    // Must be called before any drawing; safe to call before WiFi init.
    static void begin() {
        tft.init();
        tft.invertDisplay(true);
        tft.setRotation(0);
        // NO initDMA(), DELIBERATELY - this is what broke the display on Arduino core 3.
        // initDMA calls the IDF spi_master driver's spi_bus_initialize() on the SAME SPI3
        // peripheral TFT_eSPI's raw register path writes through; on ESP-IDF 5 that
        // reconfigures the controller out from under the raw path, and every write after
        // it - fillScreen, the LVGL flushes, everything - silently goes nowhere. Proven
        // on hardware: with initDMA the panel accepts writes only DURING tft.init(); with
        // it removed the full pipeline works. The flush costs nothing by going PIO: the
        // banded flush always did pushImageDMA immediately followed by dmaWait(), so the
        // DMA never overlapped rendering in the first place. See LvglDisplay.h flushCb.

        // tft.init() calls digitalWrite(BL_PIN) which detaches it from LEDC.
        // Re-attach after init so setBrightness() works via PWM.
        // Arduino core 3.x API: ledcAttachChannel replaces ledcSetup+ledcAttachPin. The
        // explicit channel is kept - the original firmware drives channel 0 and the web
        // brightness path documents that contract.
        ledcAttachChannel(PIN_TFT_BL, BL_PWM_FREQ, BL_PWM_RES, BL_PWM_CHAN);
        // display_boot_task writes 0xff immediately after attaching the pin. On this
        // active-low panel that means the backlight is OFF while the display initialises.
        setBrightness(255);
        tft.fillScreen(TFT_BLACK);

        // Allocate the band buffer NOW, before WiFi starts. WiFi fragments the heap
        // (max contiguous drops from ~110 KB to ~41 KB once connected), so a band
        // allocated after WiFi fails. Allocated once here and kept for the device's
        // lifetime - the same reasoning that puts the LVGL draw buffer in .bss.
        ensureBand(/*force=*/true);
    }

    // -- Banded full-screen rendering (original-firmware architecture) -----------
    // Allocates the shared 240xBAND_H band sprite on demand. ~38 KB, allocated
    // once and reused for every band of every frame. Returns false on OOM.
    // THE OLD RENDERER IS RETIRED. This used to createSprite() a 240x80 TFT_eSprite -
    // 38,400 bytes of HEAP - and every app drew into it through renderApp().
    //
    // It cannot coexist with LVGL. LVGL's static draw buffer (38,400 B) plus its pool
    // (16,384 B) live in .bss, which is the SAME DRAM the heap comes from, so bringing
    // LVGL in left 43 KB free with a 40,948-byte largest block - and this 38,400-byte
    // allocation then either failed or left nothing for WiFi/TLS. The device hung right
    // after "[heap] connected pre-sprite: free=43084 max=40948".
    //
    // Returning false makes renderApp() a safe no-op for any app not yet ported, so the
    // firmware stays bootable during the port instead of dying on the first draw.
    // `force` is used exactly once, from begin(), to claim the band at boot while the heap
    // is still pristine (blk8 ~110 KB). Without it the guard below early-returns at boot
    // (_appBandActive is false until an app enters), which silently defeated the
    // "allocate before WiFi" intent above: the band ended up being carved out of an
    // already-fragmented heap on Picture/Idea entry instead.
    static bool ensureBand(bool force = false) {
        if (_band.created()) return true;
        if (!force && !ALLOW_LEGACY_SPRITE && !_appBandActive) return false;
        // ZERO heap: adopt LVGL's static .bss band buffer instead of calloc'ing a second
        // 38,400-byte copy. Sizes must agree - both are 240 x BAND_H, 16 bpp.
        static_assert(sizeof(Lv::drawBuf) >= (size_t)TFT_WIDTH * BAND_H * 2,
                      "Lv::drawBuf is smaller than the 240xBAND_H 16bpp band");
        _band.adoptStatic(reinterpret_cast<uint16_t*>(Lv::drawBuf), TFT_WIDTH, BAND_H);
        _bandPtr = reinterpret_cast<uint16_t*>(Lv::drawBuf);
        if (!_band.created()) {
            Serial.printf("[disp] band adopt FAILED\n");
            _bandPtr = nullptr;
            return false;
        }
        Serial.printf("[disp] band 240x%d adopted Lv::drawBuf (0 heap) free=%u max=%u\n",
                      BAND_H, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return true;
    }

    // DELIBERATELY A NO-OP. The band is claimed once at boot (begin()) and kept for the
    // device's lifetime. TFT_eSprite has no set-external-buffer API, so "allocate once while
    // the heap is pristine and never release" is the closest we get to a .bss buffer - and it
    // has the same property that matters:
    // the heap is never split by a 38 KB hole appearing and vanishing at runtime.
    //
    // Why this matters (measured): releasing and re-taking the band per app entry left only
    // ~12 KB contiguous while the Picture app ran (band 38 KB + GIF decoder ~24 KB), and
    // web serving on top of that starved the WiFi driver - which allocates its RX/TX
    // buffers on demand - until the device dropped OFF THE NETWORK entirely ("destination
    // host unreachable") while its app loop kept running. That is the bug this fixes.
    static void freeBand() { /* intentionally never released - see comment above */ }

    // Render a full 240x240 frame in horizontal bands and DMA-push each band.
    // Same band loop the LVGL flush callback runs (see LvglDisplay.h).
    //
    // drawCb(g) is invoked once per band and draws the WHOLE 240x240 screen using
    // ABSOLUTE screen coordinates - no y-offset maths required in app code. That
    // works because we set a viewport with a negative y datum: TFT_eSprite adds
    // _yDatum to every coordinate and clips to the band, so drawing outside the
    // current band is discarded cheaply.
    //
    // The background is cleared by US, before the viewport is applied.
    // *** drawCb MUST NOT call fillSprite(). *** setViewport() stores the viewport
    // height UNCLIPPED (240), and for the y0=0 band the datum is (0,0), which makes
    // fillSprite take its fast path: memset(_img, c, _iwidth * _yHeight * 2) =
    // 240*240*2 = 115,200 bytes into a 240*BAND_H buffer - heap corruption that
    // manifests as LoadProhibited crashes in unrelated FreeRTOS code. Clearing here
    // with the viewport reset keeps _yHeight == BAND_H, so the memset is exact.
    template <typename Fn>
    static void renderFrame(Fn drawCb, uint16_t bg = TFT_BLACK) {
        if (!ensureBand()) return;
        // A TFT_eSprite already stores its pixels in the panel's byte order, so the band must
        // be pushed with swap DISABLED. The main tft's _swapBytes is left true by the LVGL
        // flush path (Lv::flushCb), and pushImage honours that global flag (it pushes through
        // pushPixels) - so without forcing it false here every band colour was swapped: Idea's
        // green matrix rain (0x07E0) came out red (0xE007) and its coral second hand (0xEBEF)
        // came out yellow (0xEFEB). Restored to true after the frame so LVGL is unaffected.
        const bool prevSwap = tft.getSwapBytes();
        tft.setSwapBytes(false);
        tft.startWrite();
        for (int16_t y0 = 0; y0 < TFT_HEIGHT; y0 += BAND_H) {
            int16_t h = min<int16_t>(BAND_H, TFT_HEIGHT - y0);
            _band.resetViewport();
            _band.fillSprite(bg);            // safe: _yHeight == BAND_H here
            // Shift the drawing origin up by y0 so absolute coords land in the band.
            _band.setViewport(0, -y0, TFT_WIDTH, TFT_HEIGHT, true);
            // Publish the band's absolute Y range so expensive per-pixel blitters
            // can skip rows outside it. The callback runs once PER BAND, so without
            // this a 128x128 icon walks all 16k of its pixels three times per frame;
            // that wasted work is what slows the launcher slide enough to tear.
            _bandY0 = y0; _bandY1 = y0 + h;
            drawCb(_band);
            _band.resetViewport();
            // PIO, matching Lv::flushCb. THIS MUST NOT GO BACK TO pushImageDMA: begin() no
            // longer calls initDMA (see the note there), so DMA_Enabled stays false and
            // pushImageDMA/dmaWait BOTH return immediately at their first line - silently,
            // no error, no crash. That left every direct-draw app blank: Picture showed
            // neither photos nor its own "No images" screen, and Idea drew nothing at all,
            // while every LVGL app looked perfect because flushCb had already been moved.
            tft.pushImage(0, y0, TFT_WIDTH, h, _bandPtr);
        }
        tft.endWrite();
        tft.setSwapBytes(prevSwap);   // leave the global flag as LVGL expects it
    }

    // App-facing full-screen render. Apps draw their entire 240x240 layout in
    // absolute coordinates; this replaces the old big-sprite + push() pattern.
    // The band is pre-cleared to bg - do NOT call fillSprite() inside drawCb.
    //
    // *** drawCb MUST BE PURE *** - it is invoked once per band (TFT_HEIGHT/BAND_H
    // times per frame). Mutating animation/state inside it advances that state 3x
    // per frame. Do state updates before calling renderApp, then only draw here.
    template <typename Fn>
    static void renderApp(Fn drawCb, uint16_t bg = TFT_BLACK) { renderFrame(drawCb, bg); }

    // Full-screen boot/status screen. Layout (all LV_ALIGN_TOP_MID, y from the top):
    //   y=18  title "SmolTV PRO"      colour 0x86FF
    //   y=54  88x88 spinner, arc w=8   track 0x19C9, indicator 0x6F7F
    //   y=168 status line 1            colour 0xF7DF
    //   y=198 status line 2            colour 0x7F9F
    // The spinner is an lv_spinner with time=1000 ms and arc_length=80 degrees, so the
    // 80-degree indicator completes one revolution per second. Pass the absolute time in
    // ms; the angle is derived from it, which keeps the motion smooth and correctly paced
    // regardless of how often this is called.
    static void showStatus(const char* line1, const char* line2 = "",
                           uint32_t nowMs = 0,
                           const char* title = DEVICE_NAME) {
        const int cx = TFT_WIDTH / 2;
        const int cy = SPIN_Y + SPIN_SIZE / 2;              // spinner centre
        const int rOut = SPIN_SIZE / 2;                     // 44
        const int rIn  = rOut - SPIN_W;                     // 36
        // The spinner runs TWO infinite 1000 ms animations (constructor):
        //   END   angle: arc_length -> arc_length + 360, path = LINEAR (no path_cb set)
        //   START angle: 0 -> 360,   path = lv_anim_path_ease_in_out
        // Because the start angle is eased and the end angle is linear, the arc LENGTH
        // breathes (grows then shrinks) instead of staying a fixed 80 degrees - which my
        // first version got wrong. Background arc is a full ring, lv_arc_set_bg_angles(0,360),
        // and lv_arc_set_rotation(270) puts 0 degrees at 12 o'clock (our -90 in _arcPixel).
        const uint32_t t1024 = (uint32_t)((nowMs % SPIN_PERIOD_MS) * 1024UL / SPIN_PERIOD_MS);
        const int startAng = (int)((360UL * _easeInOut1024(t1024)) >> 10);
        const int endAng   = SPIN_ARC_DEG + (int)((360UL * t1024) >> 10);

        renderFrame([&](TFT_eSprite& g) {
            _drawSpinner(g, cx, cy, rIn, rOut, startAng, endAng,
                         SPIN_TRACK_C, SPIN_IND_C, BOOT_BG);

            g.setTextDatum(TC_DATUM);                       // TOP_MID alignment
            g.setTextFont(4);
            g.setTextColor(BOOT_TITLE_C);
            g.drawString(title, cx, BOOT_TITLE_Y);

            g.setTextFont(4);
            g.setTextColor(BOOT_L1_C);
            g.drawString(line1, cx, BOOT_L1_Y);
            if (line2 && *line2) {
                g.setTextColor(BOOT_L2_C);
                g.drawString(line2, cx, BOOT_L2_Y);
            }
        }, BOOT_BG);
    }



    // LVGL's fixed-point cubic bezier (t and result both 0..1024).
    static uint32_t _bezier3(uint32_t t, uint32_t u0, uint32_t u1, uint32_t u2, uint32_t u3) {
        uint32_t tr  = 1024 - t;
        uint32_t tr2 = (tr * tr) >> 10;
        uint32_t tr3 = (tr2 * tr) >> 10;
        uint32_t t2  = (t * t) >> 10;
        uint32_t t3  = (t2 * t) >> 10;
        return ((tr3 * u0) >> 10) + ((3 * tr2 * t * u1) >> 20)
             + ((3 * tr * t2 * u2) >> 20) + ((t3 * u3) >> 10);
    }

    // lv_anim_path_ease_in_out: lv_bezier3(t, 0, 50, 952, 1024).
    static uint32_t _easeInOut1024(uint32_t t1024) {
        return _bezier3(t1024, 0, 50, 952, 1024);
    }

    // Blend two RGB565 colours; a = 0..256 weight of `fg`.
    static uint16_t _blend565(uint16_t bg, uint16_t fg, int a) {
        if (a <= 0)   return bg;
        if (a >= 256) return fg;
        int br = (bg >> 11) & 0x1F, bgn = (bg >> 5) & 0x3F, bb = bg & 0x1F;
        int fr = (fg >> 11) & 0x1F, fgn = (fg >> 5) & 0x3F, fb = fg & 0x1F;
        int r = br + (((fr - br) * a) >> 8);
        int gg = bgn + (((fgn - bgn) * a) >> 8);
        int b = bb + (((fb - bb) * a) >> 8);
        return (uint16_t)((r << 11) | (gg << 5) | b);
    }


    // ANTI-ALIASED spinner: full track ring plus the indicator arc, composited per pixel.
    // Drawing one radial line per degree (the previous approach) leaves stair-stepping on
    // both the ring edges and the arc ends - the reported "jagged, not smooth". LVGL's arc
    // is anti-aliased, so we compute per-pixel coverage instead. TFT_eSPI's own
    // drawSmoothArc is unusable here: its float wedge/spot helpers overflow the 8 KB
    // loopTask stack when called on a sprite.
    //
    // Angles are LVGL-style: 0 deg at 12 o'clock, increasing clockwise. `endAng` may exceed
    // 360 (the end-angle animation runs to arc_length+360); it is normalised per pixel.
    static void _drawSpinner(TFT_eSprite& g, int cx, int cy, int rIn, int rOut,
                             int startAng, int endAng,
                             uint16_t track, uint16_t ind, uint16_t bg) {
        const float fIn = rIn - 0.5f, fOut = rOut + 0.5f;
        const int   lo = (int)(fIn * fIn), hi = (int)(fOut * fOut);
        int sA = ((startAng % 360) + 360) % 360;
        int span = endAng - startAng; if (span < 0) span = 0; if (span > 360) span = 360;

        for (int y = cy - rOut - 1; y <= cy + rOut + 1; y++) {
            for (int x = cx - rOut - 1; x <= cx + rOut + 1; x++) {
                int dx = x - cx, dy = y - cy;
                int d2 = dx * dx + dy * dy;
                if (d2 > hi || d2 < lo) continue;          // cheap integer reject
                float d = sqrtf((float)d2);
                // Radial coverage: 1 inside, feathered over the outer half-pixel.
                float cov = fminf(d - fIn, fOut - d);
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                // Angle of this pixel, 0 at top, clockwise.
                float ang = atan2f((float)dx, (float)-dy) * 57.29578f;
                if (ang < 0) ang += 360.0f;
                // Position within the indicator span, with half-degree end feathering.
                float rel = ang - (float)sA;
                if (rel < 0) rel += 360.0f;
                float iCov = 0.0f;
                if (rel <= (float)span) {
                    iCov = fminf(rel, (float)span - rel);   // distance to nearer end
                    iCov = iCov > 1.0f ? 1.0f : iCov;
                }
                uint16_t c = _blend565(bg, track, (int)(cov * 256));
                if (iCov > 0.0f) c = _blend565(c, ind, (int)(cov * iCov * 256));
                g.drawPixel(x, y, c);
            }
        }
    }

    // THE PANEL IS ACTIVE-LOW: a HIGHER duty is DIMMER, and the duty is written DIRECTLY,
    // with no inversion. Measured, not assumed - with the inversion removed, brt=220 renders
    // visibly dimmer than brt=75, and two units set to a matched 75 are indistinguishable.
    // That also makes the boot ladder read correctly: duty 0xff is the backlight OFF during
    // init, and the 254 -> 159 walk (unit steps, 40 ms each) is a fade-IN. Consistent with
    // TFT_BACKLIGHT_ON=LOW in platformio.ini.
    //
    // WHY THE INVERSION HID FOR SO LONG: this unit's brt.json held 180 and the comparison
    // unit's held 75, and 255 - 180 == 75, so the inverted code emitted exactly the same duty
    // and the two looked identical in every side-by-side. It only showed up once the two
    // values were varied independently.
    //
    // The brt.json default of 180 is therefore a DIM default.
    // Raw backlight write (active-low panel: higher duty = dimmer; no inversion). Used by
    // night mode to dim/undim each tick and by the boot fade - it does NOT change the stored
    // day level, so night dimming can't overwrite it.
    static void setBrightness(uint8_t value) {
        // Core 3.x: ledcWrite() takes the PIN; ledcWriteChannel keeps channel semantics.
        ledcWriteChannel(BL_PWM_CHAN, value);
    }

    // The DAY brightness, held in RAM: written by the boot brt.json read and by a web
    // brightness save, and read by night mode every tick as the "not dimmed" level. Keeping
    // it here avoids the per-tick brt.json flash read the previous night-mode code did, and
    // lets a live brightness change take effect immediately.
    static void setDayBrightness(uint8_t value) {
        _dayBright = value;
        ledcWriteChannel(BL_PWM_CHAN, value);
    }
    static uint8_t dayBrightness() { return _dayBright; }

    // Draw a one-line status message centred on the TFT directly (no sprite).
    // Use during the pre-WiFi boot phase before createFramebuffer() is called.
    static void status(const char* msg) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString(msg, 120, 120);
    }




    static BandSprite  _band;    // 240xBAND_H view onto Lv::drawBuf (no heap)
    static uint16_t*   _bandPtr; // pixel buffer returned by _band.createSprite()

private:
    // One definition, in Config.h. This used to repeat the literal, so the default lived in
    // two places and only one of them was findable from brt.json's reader.
    static inline uint8_t _dayBright = BRT_DEFAULT;
    Display() = delete;
};

// C++17 inline variables. These were plain definitions, which only linked because this
// header reached exactly one translation unit (main.cpp). SyncWeb.cpp now also pulls it in
// (via WebRoutes.h, for the brightness setters), so without `inline` the linker reports
// multiple definition of Display::tft/_band/_bandPtr.
inline TFT_eSPI    Display::tft;
inline BandSprite  Display::_band(&Display::tft);
inline uint16_t*   Display::_bandPtr = nullptr;
