#pragma once
#include <AnimatedGIF.h>
#include <new>

// -- Shared on-demand GIF decoder ----------------------------------------------
// An AnimatedGIF object is ~24 KB (LZW dictionary + ucFileBuf + line buffers).
// We previously held TWO as static members (AppPicture::_gif, AppWeather::_wGif),
// permanently resident in DRAM = ~47 KB wasted, even though only one app runs at
// a time. That was the single largest static consumer in the build.
//
// So: one shared instance, allocated on app entry and destroyed on exit.
//
// Only one app is active at a time, so the refcount is effectively 0 or 1; it is
// kept for safety if two consumers ever overlap.
class GifDec {
public:
    // Returns the shared decoder, allocating it if needed. nullptr on OOM.
    static AnimatedGIF* acquire() {
        if (!_gif) {
            _gif = new (std::nothrow) AnimatedGIF();
            Serial.printf("[gif] acquire %s free=%u max=%u\n",
                          _gif ? "ok" : "OOM", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            if (!_gif) return nullptr;
            // *** begin() IS MANDATORY - omitting it is a guaranteed crash. ***
            // It is the ONLY place AnimatedGIF sets `pLineBufAligned` (AnimatedGIF.cpp:288).
            // Without it that pointer stays NULL, and the very first scanline decode does
            //     buf = pPage->pLineBufAligned + (iWidth - iXCount);   // gif.inl:1426
            //     *(BIGUINT *)buf = tmp;
            // i.e. a store through NULL -> "Guru Meditation Error: StoreProhibited,
            // EXCVADDR 0x00000000" inside GIFMakePels. That was the Weather-app reboot, and
            // it hit every GIF consumer: the Picture app could never display a GIF either.
            // GIF_PALETTE_RGB565_LE (= 0) yields host-order uint16_t, which is what
            // TFT_eSPI::drawPixel expects - so callers must NOT byte-swap the palette.
            _gif->begin(GIF_PALETTE_RGB565_LE);
        }
        _refs++;
        return _gif;
    }

    // Drops a reference; frees the decoder when the last user releases it.
    static void release() {
        if (_refs > 0) _refs--;
        if (_refs == 0 && _gif) {
            delete _gif;
            _gif = nullptr;
            Serial.printf("[gif] freed free=%u max=%u\n",
                          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        }
    }

private:
    static AnimatedGIF* _gif;
    static int          _refs;
};

inline AnimatedGIF* GifDec::_gif  = nullptr;
inline int          GifDec::_refs = 0;
