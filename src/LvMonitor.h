#pragma once
#include <lvgl.h>
#include "LvFontsGen.h"
#include "LvglDisplay.h"

// -- The Monitor screen ------------------------------------------------------------------
//
// One deliberate omission worth naming: this screen does NOT re-initialise the global LVGL
// theme on entry (lv_theme_default_init + lv_disp_set_theme). Every label below sets its
// own font explicitly (BODY27), so the theme's default font is unobservable here, and
// re-initialising the global theme from inside one app would change every OTHER screen's
// defaults for the rest of the run. If a later screen turns out to depend on the theme font
// being SMALL16, revisit this rather than assuming it does not matter.
//
// ALL COLOURS ARE RGB565 - use Lv::c565.
namespace LvMonitor {

inline lv_obj_t* screen   = nullptr;   // root
inline lv_obj_t* panel    = nullptr;   // 230x230
inline lv_obj_t* card     = nullptr;   // 220x110
inline lv_obj_t* lblTemp  = nullptr;   // CPU temp
inline lv_obj_t* lblPower = nullptr;   // CPU power
inline lv_obj_t* lblUsage = nullptr;   // CPU usage
inline lv_obj_t* lblClock = nullptr;   // CPU clock
inline lv_obj_t* chart    = nullptr;
inline lv_chart_series_t* series = nullptr;
inline lv_obj_t* lblRam   = nullptr;
inline lv_obj_t* barRam   = nullptr;
inline lv_obj_t* lblDisk  = nullptr;
inline lv_obj_t* barDisk  = nullptr;

// The CPU-history series uses an EXTERNAL array (lv_chart_set_ext_y_array).
static constexpr int CHART_POINTS = 40;
inline lv_coord_t cpuHistory[CHART_POINTS];

// Layout widths shared by more than one widget, so a change lands everywhere it should.
// PANEL_SIZE is the square root panel; CONTENT_W is the width the card and both bars share.
static constexpr lv_coord_t PANEL_SIZE = 230;
static constexpr lv_coord_t CONTENT_W  = 220;

// Placeholder texts, read BYTE-EXACT from. Note "55 deg C" uses a real UTF-8 degree
// sign (C2 B0) - an older comment transcribed it as "55*C", which is wrong. Also note the
// disk placeholder has THREE spaces.
inline const char* const INIT_TEMP  = "55\u00B0C";
inline const char* const INIT_USAGE = "CPU: 8%";
inline const char* const INIT_CLOCK = "2395MHZ";
inline const char* const INIT_RAM   = "RAM: 90%";
inline const char* const INIT_DISK  = "C:\\   0G";
inline const char* const INIT_POWER = "--";             // the shared "--"+0x12

// Update formats, also byte-exact. "%d deg C" is again a real degree sign,
// and the disk format has TWO spaces before "Free".
inline const char* const FMT_USAGE = "CPU:%d%%";
inline const char* const FMT_POWER = "%.1fW";
inline const char* const FMT_TEMP  = "%d\u00B0C";
inline const char* const FMT_CLOCK = "%dMHZ";
inline const char* const FMT_RAM   = "RAM:%d%%";
inline const char* const FMT_DISK  = "C:\\ %d%%  Free %dGB";

// Threshold colours.
//   CPU  - TWO levels: < 81 -> 0x0FB8, else 0xF941. Applied to the usage LABEL colour AND,
//          by writing series->color directly, to the chart line.
//   RAM and DISK - THREE levels: < 51 -> 0x07C1, 51..80 -> 0xFD20, >= 81 -> 0xF941.
//          Applied to the bar's INDICATOR bg_color.
inline uint16_t cpuColour(int pct)  { return pct < 81 ? 0x0FB8 : 0xF941; }
inline uint16_t loadColour(int pct) {
    if (pct >= 81) return 0xF941;
    if (pct >= 51) return 0xFD20;
    return 0x07C1;
}

inline void build() {
    if (screen) { Lv::adoptScreen(screen, &screen); return; }
    for (int i = 0; i < CHART_POINTS; i++) cpuHistory[i] = 0;

    // ROOT: bg 0, clear SCROLLABLE. Nothing else.
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, Lv::c565(0x0000), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // PANEL 230x230 centred, bg 0 opa 0xFF, border 0.
    panel = lv_obj_create(screen);
    lv_obj_set_style_bg_color(panel, Lv::c565(0x0000), 0);
    lv_obj_set_width(panel, PANEL_SIZE);
    lv_obj_set_height(panel, PANEL_SIZE);
    lv_obj_set_align(panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);

    // CARD 220x110 at (0, -54). Only the border colour and opacity are set - the border
    // WIDTH and the background come from the theme.
    card = lv_obj_create(panel);
    lv_obj_set_width(card, CONTENT_W);
    lv_obj_set_height(card, 110);
    lv_obj_set_x(card, 0);
    lv_obj_set_y(card, -54);
    lv_obj_set_align(card, LV_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_color(card, Lv::c565(0x0FC7), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);

    auto mkLabel = [](lv_obj_t* parent, int x, int y, const char* text) {
        lv_obj_t* l = lv_label_create(parent);
        lv_obj_set_width(l, LV_SIZE_CONTENT);
        lv_obj_set_height(l, LV_SIZE_CONTENT);
        lv_obj_set_x(l, x);
        lv_obj_set_y(l, y);
        lv_obj_set_align(l, LV_ALIGN_CENTER);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, &FONT_BODY27, 0);
        return l;
    };

    lblTemp  = mkLabel(card,  69, -38, INIT_TEMP);    // (+69, -38)
    lblPower = mkLabel(card,  69, -20, INIT_POWER);   // (+69, -20)
    lv_obj_set_style_text_color(lblPower, Lv::c565(0xF221), 0);
    lblUsage = mkLabel(card,   -58, -39, INIT_USAGE);   // (-58, -39)
    lblClock = mkLabel(card,   -54, -20, INIT_CLOCK);   // (-54, -20)
    lv_obj_set_style_text_color(lblClock, Lv::c565(0xFE81), 0);
    lv_obj_set_style_text_opa(lblClock, LV_OPA_COVER, 0);

    // CHART 214x70 at (-1, +23), LINE, 40 points, no division lines.
    chart = lv_chart_create(card);
    lv_obj_set_width(chart, 214);
    lv_obj_set_height(chart, 70);
    lv_obj_set_x(chart, -1);
    lv_obj_set_y(chart, 23);
    lv_obj_set_align(chart, LV_ALIGN_CENTER);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 10, 5, 1, 1, false, 50);
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 10, 5, 5, 2, false, 50);
    series = lv_chart_add_series(chart, Lv::c565(0x07FF), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_ext_y_array(chart, series, cpuHistory);
    lv_obj_set_style_line_color(chart, Lv::c565(0x0FB8), LV_PART_ITEMS);
    lv_obj_set_style_line_opa(chart, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_line_width(chart, 1, LV_PART_ITEMS);
    // Same trick as Stock: zero width AND height on LV_PART_INDICATOR hides the point dots.
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 0, LV_PART_TICKS);

    // RAM label + bar. The bars are children of the PANEL, not the card.
    lblRam = mkLabel(panel, -61, 28, INIT_RAM);       // (-61, +28)
    lv_obj_set_style_text_color(lblRam, Lv::c565(0xFFDF), 0);
    lv_obj_set_style_text_opa(lblRam, LV_OPA_COVER, 0);

    barRam = lv_bar_create(panel);
    lv_bar_set_value(barRam, 90, LV_ANIM_OFF);        // 90 at build time
    lv_obj_set_width(barRam, CONTENT_W);
    lv_obj_set_height(barRam, 10);
    lv_obj_set_x(barRam, -2);
    lv_obj_set_y(barRam, 47);
    lv_obj_set_align(barRam, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(barRam, 4, 0);
    lv_obj_set_style_bg_color(barRam, Lv::c565(0xADB6), 0);
    lv_obj_set_style_bg_opa(barRam, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(barRam, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(barRam, Lv::c565(0xF221), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(barRam, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(barRam, 0, LV_PART_INDICATOR);

    // Disk label + bar. NOTE the disk bar does NOT get the border_width 0 the RAM bar does.
    //
    // x is nudged from the Ghidra-verified original (-15) to 0 - a deliberate deviation, not
    // parity. This label is LV_SIZE_CONTENT width under a plain LV_ALIGN_CENTER (mkLabel), so
    // it grows symmetrically about its anchor with no bound; at -15 the anchor sits 15px left
    // of true screen-centre, so any rendered text >=210px wide (routine once free space or
    // usage% needs an extra digit) pushes its LEFT edge past the physical screen edge. x=0
    // moves the anchor back to centre, raising that threshold to >=240px - it does not
    // eliminate the possibility for very long text, but it covers the common case.
    lblDisk = mkLabel(panel, 0, 75, INIT_DISK);     // (0, +75) - was (-15, +75)

    barDisk = lv_bar_create(panel);
    lv_bar_set_value(barDisk, 50, LV_ANIM_OFF);       // 50 at build time
    lv_obj_set_width(barDisk, CONTENT_W);
    lv_obj_set_height(barDisk, 10);
    lv_obj_set_x(barDisk, -2);
    lv_obj_set_y(barDisk, 96);
    lv_obj_set_align(barDisk, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(barDisk, 4, 0);
    lv_obj_set_style_bg_color(barDisk, Lv::c565(0xADB6), 0);
    lv_obj_set_style_bg_opa(barDisk, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(barDisk, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(barDisk, Lv::c565(0x0FB8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(barDisk, LV_OPA_COVER, LV_PART_INDICATOR);

    Lv::adoptScreen(screen, &screen);
    Lv::logMem("monitor");
}

// There is NO "connecting" or "no data" state. The build-time literals are not a resting
// state either: the app zeroes its context and calls this immediately on entry, so with no
// AIDA64 peer the screen settles on zeros rather than on "CPU: 8% / 2395MHZ / 55C".
inline void update(int cpuMhz, int cpuTempC, float cpuPowerW, int cpuPct,
                   int ramPct, int diskPct, int diskFreeGb) {
    if (!screen) return;
    lv_label_set_text_fmt(lblUsage, FMT_USAGE, cpuPct);
    lv_label_set_text_fmt(lblPower, FMT_POWER, cpuPowerW);
    lv_label_set_text_fmt(lblTemp,  FMT_TEMP,  cpuTempC);
    lv_label_set_text_fmt(lblClock, FMT_CLOCK, cpuMhz);
    lv_label_set_text_fmt(lblRam,   FMT_RAM,   ramPct);
    lv_label_set_text_fmt(lblDisk,  FMT_DISK,  diskPct, diskFreeGb);

    // series->color is written DIRECTLY rather than through a setter, and the usage label
    // takes the same value.
    const uint16_t cpuCol = cpuColour(cpuPct);
    if (series) series->color = Lv::c565(cpuCol);
    lv_obj_set_style_text_color(lblUsage, Lv::c565(cpuCol), 0);

    lv_obj_set_style_bg_color(barRam,  Lv::c565(loadColour(ramPct)),  LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(barDisk, Lv::c565(loadColour(diskPct)), LV_PART_INDICATOR);

    // Both bars animate.
    lv_bar_set_value(barRam,  ramPct,  LV_ANIM_ON);
    lv_bar_set_value(barDisk, diskPct, LV_ANIM_ON);

    // Shift the CPU history and re-point the series at the external array.
    for (int i = 0; i < CHART_POINTS - 1; i++) cpuHistory[i] = cpuHistory[i + 1];
    cpuHistory[CHART_POINTS - 1] = (lv_coord_t)cpuPct;
    lv_chart_set_ext_y_array(chart, series, cpuHistory);
    lv_chart_refresh(chart);
}

} // namespace LvMonitor
