#pragma once
#include <Arduino.h>

// Abstract base for all display apps.
class AppBase {
public:
    virtual ~AppBase() = default;

    // Called once when this app becomes active.
    virtual void onEnter() {}

    // Called once when this app is deactivated.
    virtual void onExit() {}

    // Called every loop() tick. Returns false if the app wants to be skipped.
    virtual bool loop() = 0;

    // Reload config from LittleFS (called after web UI saves settings).
    virtual void reloadConfig() {}

    // Called on long press while this app is active (sub-navigation, settings).
    virtual void onLongPress() {}

    // Tap delivery: the launcher maintains ONE monotonic tap counter and each app LATCHES
    // and DIFFS it, using the sign of the delta for direction. Apps that care override this;
    // the default ignores taps, as Monitor does (it never reads the counter).
    virtual void onTapCounter(int /*counter*/) {}

    virtual const char* name() const = 0;

    // True for GuiLite-style full-screen pixel apps (Idea, Picture) that draw via the
    // banded TFT path (Display::renderApp) instead of building an LVGL screen. AppCtrl
    // allocates the 240x80 band on enter, frees it on exit, and - crucially - SKIPS the
    // central lv_timer_handler pump while such an app is active, so LVGL's flush and the
    // app's DMA band-pushes don't fight over the SPI bus.
    virtual bool usesDirectDraw() const { return false; }
};
