#pragma once
#include <time.h>

// Reading the local wall clock without Arduino's getLocalTime() race.
//
// getLocalTime(&t, ms) is implemented as:
//
//     uint32_t start = millis();
//     while ((millis() - start) <= ms) { time(&now); localtime_r(&now, info); ... }
//     return false;
//
// With ms = 0 -- the non-blocking form every caller here wants, because the default 5 s
// form stalls the render loop -- the loop body only runs while millis() has not ticked. If
// the millisecond boundary falls between `start = millis()` and the `while` test, the body
// never executes: `info` is left COMPLETELY UNFILLED and the function returns false, even
// though the clock is perfectly valid.
//
// That is a live coin flip on every call, and it produced a visible defect: the weather
// screen's seconds counter dropped to 00 at random, because the caller substituted 0 for
// every field when the call "failed". The brightness scheduler had the same bug, snapping
// the backlight to daytime brightness for one frame at night.
//
// The fix is to drop the success flag entirely: read the fields unconditionally, so there is
// no path that can substitute zero.
//
// timeIsSet() applies the same "is the clock actually set?" test getLocalTime uses
// internally, so callers keep a genuine not-yet-synced signal while losing the race.

// Fill `out` with the current local time. Always succeeds in the sense that `out` is always
// written; use timeIsSet() to ask whether the clock has been synced yet.
static inline void timeNow(struct tm& out) {
    time_t now = time(nullptr);
    localtime_r(&now, &out);
}

// True once the clock holds a plausible wall-clock date. Mirrors getLocalTime's own check,
// which treats any year past 2016 as synced.
static inline bool timeIsSet(const struct tm& t) {
    return t.tm_year > (2016 - 1900);
}

// Convenience for the common "snapshot and tell me if it is real" call.
static inline bool timeNowIfSet(struct tm& out) {
    timeNow(out);
    return timeIsSet(out);
}
