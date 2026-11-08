#pragma once
#include <Arduino.h>
#include "Config.h"

#ifndef USE_DIGITAL_BUTTON
#define USE_DIGITAL_BUTTON 0
#endif

#ifndef PIN_BUTTON
#define PIN_BUTTON 0 // GPIO0 (BOOT button on Spotpear ESP32-S3 1.54" MUMA)
#endif

#ifndef HAS_CST816_TOUCH
#define HAS_CST816_TOUCH 0
#endif

#if HAS_CST816_TOUCH
#include <Wire.h>
#endif

// Capacitive touch / CST816D touchscreen / physical button handler.
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
#if HAS_CST816_TOUCH
        pinMode(TP_INT_PIN, INPUT_PULLUP);
        pinMode(TP_RST_PIN, OUTPUT);
        digitalWrite(TP_RST_PIN, LOW);
        delay(10);
        digitalWrite(TP_RST_PIN, HIGH);
        delay(50);
        Wire1.begin(TP_SDA_PIN, TP_SCL_PIN, 400000);
        Wire1.beginTransmission(0x15);
        _cst816Present = (Wire1.endTransmission() == 0);
        Serial.printf("[touch] CST816D I2C touchscreen (SDA=%d, SCL=%d, INT=%d): %s\n",
                      TP_SDA_PIN, TP_SCL_PIN, TP_INT_PIN,
                      _cst816Present ? "detected" : "not detected");
#endif
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
        bool btnDown = (digitalRead(PIN_BUTTON) == LOW);
        bool scrDown = false;
#if HAS_CST816_TOUCH
        if (_cst816Present) {
            Wire1.beginTransmission(0x15);
            Wire1.write(0x02); // Finger num register
            if (Wire1.endTransmission(false) == 0 && Wire1.requestFrom((uint8_t)0x15, (uint8_t)1) == 1) {
                uint8_t fingers = Wire1.read() & 0x0F;
                scrDown = (fingers > 0);
            }
        }
#endif
        bool touched = btnDown || scrDown;
        _lastRaw = touched ? 0 : 1;
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
#if HAS_CST816_TOUCH
    static inline bool _cst816Present = false;
#endif
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
