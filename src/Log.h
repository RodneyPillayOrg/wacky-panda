#pragma once
#include <Arduino.h>

// -- Logging: UART only ------------------------------------------------------------------
// This costs zero .bss and zero heap - one transient stack buffer per line.
//
// An earlier version kept a 1792-byte .bss ring (16x112) readable over HTTP at /log, plus an
// RTC_NOINIT crash capture, on the premise that "this unit is flashed over OTA and its USB
// serial line is not carrying data, so a UART-only log is unreadable". That premise was
// FALSE: the silence was the pyserial DTR/RTS auto-reset gotcha - opening the port asserted
// EN/GPIO0 and held the chip in reset. Open with DTR/RTS de-asserted, pulse a reset into the
// app, and the UART streams boot and runtime logs reliably. The ring and the /log endpoint
// were removed once that was understood.
namespace Log {

inline void addf(const char* fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Serial.println(line);
}

} // namespace Log

#define LOGF(...) Log::addf(__VA_ARGS__)
