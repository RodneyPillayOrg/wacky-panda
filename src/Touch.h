#pragma once
#include <Arduino.h>
#include "Config.h"

#ifndef USE_DIGITAL_BUTTON
#define USE_DIGITAL_BUTTON 0
#endif

#ifndef PIN_BUTTON
#define PIN_BUTTON 0 // Default GPIO0 (BOOT / User button on Xiaozhi ESP32-S3 boards)
#endif

// Capacitive touch / physical button handler.
// short click  -> next app / spin / quick-stop
// double click -> previous app / quick-stop
// long press   -> enter / exit app
enum class TouchEvent { None, Short, Double, Long };

class Touch {
public:
    static void begin() {
#if USE_DIGITAL_BUTTON
        pinMode(PIN_BUTTON, INPUT_PULLUP);
        _threshold = 0;
        Serial.printf("[touch] digital button configured on GPIO %d (active LOW)\n", PIN_BUTTON);
#else
        uint16_t baseline = 0;
        for (int i = 0; i < 16; i++) {
            uint16_t r = touchRead(PIN_TOUCH);
            if (r > baseline) baseline = r;
            delay(20);
        }
        if (baseline > TOUCH_BASELINE_MARGIN + 8) {
            _threshold = baseline - TOUCH_BASELINE_MARGIN;
        } else {
            _threshold = TOUCH_THRESHOLD;
        }
        Serial.printf("[touch] baseline=%u threshold=%u (pin %d)\n",
                      baseline, _threshold, PIN_TOUCH);
#endif
        _guardUntil = millis() + TOUCH_BOOT_GUARD_MS;
    }

    static uint16_t threshold() { return _threshold; }
    static uint16_t raw() { return _lastRaw; }

    static TouchEvent poll() {
#if USE_DIGITAL_BUTTON
        _lastRaw = (uint16_t)digitalRead(PIN_BUTTON);
        bool touched = (_lastRaw == LOW);
#else
        _lastRaw = touchRead(PIN_TOUCH);
        bool touched = (_lastRaw != 0) && (_lastRaw < _threshold);
#endif
        unsigned long now = millis();

        if ((long)(now - _guardUntil) < 0) {
            _wasDown = touched;
            _taps    = 0;
            return TouchEvent::None;
        }

        if (touched && !_wasDown) {
            _wasDown    = true;
            _pressStart = now;
        } else if (!touched && _wasDown) {
            _wasDown = false;
            unsigned long held = now - _pressStart;
            if (held < TOUCH_PRESS_DEBOUNCE_MS) return TouchEvent::None;
            if (held >= TOUCH_LONG_MIN) {
                _taps = 0;
                return TouchEvent::Long;
            }
            _taps++;
            _lastTap = now;
        }

        if (!touched && _taps > 0 && (now - _lastTap) > TOUCH_CLICK_WINDOW_MS) {
            int t = _taps;
            _taps = 0;
            if (t >= 2) return TouchEvent::Double;
            return TouchEvent::Short;
        }

        return TouchEvent::None;
    }

private:
    static bool          _wasDown;
    static unsigned long _pressStart;
    static unsigned long _lastTap;
    static unsigned long _guardUntil;
    static uint16_t      _lastRaw;
    static uint16_t      _threshold;
    static int           _taps;
};

uint16_t      Touch::_lastRaw    = 0;
uint16_t      Touch::_threshold  = TOUCH_THRESHOLD;

bool          Touch::_wasDown    = false;
unsigned long Touch::_pressStart = 0;
unsigned long Touch::_lastTap    = 0;
unsigned long Touch::_guardUntil = 0;
int           Touch::_taps       = 0;
