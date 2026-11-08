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
#ifdef BOARD_SPOTPEAR_MUMA_S3
        pinMode(40, INPUT_PULLUP); // Side button K1 (Volume+)
        pinMode(41, INPUT_PULLUP); // Side button K2 (Volume-)
#endif
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
        Wire1.setTimeOut(5);
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
#if USE_DIGITAL_BUTTON
        _guardUntil = millis() + 100;
#else
        _guardUntil = millis() + TOUCH_BOOT_GUARD_MS;
#endif
    }

    static uint16_t threshold() { return _threshold; }
    static uint16_t raw() { return _lastRaw; }
    static void setImmediateSingleTap(bool en) {
        _immediateSingleTap = en;
        _wasDown = false;
        _longFired = false;
        _taps = 0;
    }

    static TouchEvent poll() {
        unsigned long now = millis();
#if USE_DIGITAL_BUTTON
        bool btnDown = (digitalRead(PIN_BUTTON) == LOW);
#ifdef BOARD_SPOTPEAR_MUMA_S3
        btnDown = btnDown || (digitalRead(40) == LOW) || (digitalRead(41) == LOW);
#endif
        bool scrDown = _lastScrDown;
#if HAS_CST816_TOUCH
        if (_cst816Present && (now - _lastI2cPollMs >= 5)) {
            _lastI2cPollMs = now;
            // Read full 6-byte CST816D touch packet (0x01..0x06) so touch state is consumed cleanly
            // and check both FingerNum (buf[1]) and EventFlag (buf[2] >> 6: 0=Down, 1=Up, 2=Contact).
            Wire1.beginTransmission(0x15);
            Wire1.write(0x01);
            if (Wire1.endTransmission(false) == 0 && Wire1.requestFrom((uint8_t)0x15, (uint8_t)6) == 6) {
                uint8_t buf[6];
                for (int i = 0; i < 6; ++i) buf[i] = Wire1.read();
                uint8_t fingers   = buf[1] & 0x0F;
                uint8_t eventFlag = (buf[2] >> 6) & 0x03; // 1 = Touch UP
                scrDown = (fingers == 1) && (eventFlag != 0x01);
            } else {
                scrDown = false;
            }
            _lastScrDown = scrDown;
        }
#endif
        bool touched = btnDown || scrDown;
        _lastRaw = touched ? 0 : 1;
#else
        _lastRaw = touchRead(PIN_TOUCH);
        bool touched = (_lastRaw != 0) && (_lastRaw < _threshold);
#endif

        if ((long)(now - _guardUntil) < 0) {
            _wasDown   = touched;
            _longFired = false;
            _taps      = 0;
            return TouchEvent::None;
        }

        if (touched && !_wasDown) {
            _wasDown    = true;
            _longFired  = false;
            _pressStart = now;
        } else if (touched && _wasDown) {
            const unsigned long longMinMs = _immediateSingleTap ? 600UL : 400UL;
            if (!_longFired && (now - _pressStart) >= longMinMs) {
                _longFired = true;
                _taps = 0;
                return TouchEvent::Long;
            }
        } else if (!touched && _wasDown) {
            _wasDown = false;
            if (_longFired) {
                _longFired = false;
                return TouchEvent::None;
            }
            unsigned long held = now - _pressStart;
#if HAS_CST816_TOUCH
            constexpr unsigned long minDebounceMs = 5;
#else
            constexpr unsigned long minDebounceMs = TOUCH_PRESS_DEBOUNCE_MS;
#endif
            if (held < minDebounceMs) return TouchEvent::None;

            // Inside Wacky Panda (_immediateSingleTap == true), fire Short tap IMMEDIATELY
            // on finger release (skipping the 220ms double-click wait) so the tap triggers
            // the Reel Spin Breathe -> Spin cleanly with zero post-tap lag!
            if (_immediateSingleTap) {
                _taps = 0;
                return TouchEvent::Short;
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
    static inline unsigned long _lastI2cPollMs = 0;
#endif
    static inline bool _lastScrDown = false;
    static inline bool _immediateSingleTap = false;
    static inline bool _longFired = false;
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
