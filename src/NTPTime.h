#pragma once
#include <Arduino.h>
#include <time.h>
#include <ArduinoJson.h>
#include "Config.h"

class NTPTime {
public:
    static void begin() {
        String server = NTP_DEFAULT;
        int tzMode = 0, tzOffset = 0;

        DynamicJsonDocument doc(256);
        if (FSUtil::readJson(PATH_NTP, doc)) server = doc["ntp"] | NTP_DEFAULT;
        if (FSUtil::readJson(PATH_TZ, doc)) {
            // readInt: tz.json is written as ints by the /set path (.toInt()), so this is
            // hardening rather than a live bug -- but a hand-edited or restored file holding
            // the string "120" would otherwise silently read as UTC.
            tzMode   = FSUtil::readInt(doc, "mode", 0);
            tzOffset = FSUtil::readInt(doc, "offset", 0);
        }

        // tz.json "offset" is in MINUTES, not hours, clamped to -720..840 (-12 h..+14 h).
        // UTC+2 is stored as {"mode":1,"offset":120}. Multiplying by 3600 treated that as 120
        // HOURS; combined with the wrong key names being written, the offset was never
        // applied at all and the clock ran 2 h slow.
        long gmtOffset = (long)tzOffset * 60L;
        configTime(gmtOffset, 0, server.c_str(), NTP_ALT1, NTP_ALT2);

        // Wait for sync (up to 10 s)
        struct tm t;
        unsigned long start = millis();
        while (!getLocalTime(&t) && millis() - start < 10000) delay(200);
    }


    // Resolve the effective UTC offset in MINUTES, honouring the tz mode exactly as the
    // original Config_getTimezone does:
    //   mode 1  = MANUAL   -> tz.json "offset" (minutes)
    //   mode 0  = AUTO (Weather city) -> the weather city's timezone from city.json "t" (hours)
    //             + "mt" (minutes), which the weather fetch populates from the API and
    //             AppWeather_owmResponseCb persists via Config_writeCity. Both carry sign, so
    //             the offset is simply t*60 + mt (e.g. UTC-5:30 -> t=-5, mt=-30 -> -330).
    // Clamped to -720..840.
    static long resolveOffsetMin() {
        DynamicJsonDocument doc(256);
        int  mode = 0;
        long off  = 0;
        if (FSUtil::readJson(PATH_TZ, doc)) {
            mode = FSUtil::readInt(doc, "mode", 0);      // see the note in begin()
            off  = FSUtil::readInt(doc, "offset", 0);
        }
        if (mode != 1) {                       // AUTO: derive from the weather city
            DynamicJsonDocument cdoc(512);
            long th = 0, tm = 0;
            if (FSUtil::readJson(PATH_CITY, cdoc)) {
                th = FSUtil::readInt(cdoc, "t",  0);
                tm = FSUtil::readInt(cdoc, "mt", 0);
            }
            off = th * 60L + tm;
        }
        if (off >  840) off =  840;
        if (off < -720) off = -720;
        return off;
    }

    // Seed the cached offset with the value applied at boot, so the first live re-apply is a
    // no-op unless the user actually changed the timezone.
    static void seedOffset(long offsetMin) { _lastOffsetMin = offsetMin; _seeded = true; }

    // Re-apply the timezone offset LIVE after a web tz save. configTime used to run only at
    // boot, so a web tz change did not take effect until reboot.
    // configTime sets the offset via setenv("TZ")+tzset synchronously, so localtime_r shifts
    // within one clock tick; the async SNTP re-sync just refreshes the UTC base. Only re-applies
    // when the offset actually changed, so unrelated /set saves don't needlessly restart SNTP.
    // Returns true if it re-applied. tz.json "offset" is MINUTES, clamped -720..840.
    static bool applyTimezoneLive() {
        long off = resolveOffsetMin();
        if (_seeded && off == _lastOffsetMin) return false;
        _lastOffsetMin = off;
        _seeded = true;
        String server = NTP_DEFAULT;
        DynamicJsonDocument ndoc(256);
        if (FSUtil::readJson(PATH_NTP, ndoc)) { String s = ndoc["ntp"] | ""; if (s.length()) server = s; }
        configTime(off * 60L, 0, server.c_str(), NTP_ALT1, NTP_ALT2);
        return true;
    }

private:
    static inline long _lastOffsetMin = 0;
    static inline bool _seeded        = false;
};
