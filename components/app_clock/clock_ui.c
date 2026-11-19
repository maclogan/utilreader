#include "clock_ui.h"

#include <stdio.h>                      // snprintf()
#include <time.h>                       // time(), gmtime_r()
#include "epd_2in13.h"                  // drawing primitives + display refresh

#define CLOCK_24H       0               // 1 = "17:05", 0 = "5:05 PM"
#define TIME_SCALE      7               // digit size: the 5x7 font at 7x = 35x49 px per digit
#define TIME_Y          22              // top of the big digits
#define DATE_Y          84              // top of the date line (2x text)
#define CLOCK_SET_AFTER 1700000000      // time() is below this until internet time has synced (Nov 2023)

// Current local time, split into fields. Returns false if the clock hasn't been set yet.
static bool local_time(int utc_offset, struct tm *out)
{
    time_t now = time(NULL);                                // seconds since 1970, UTC
    if (now < CLOCK_SET_AFTER) {
        return false;
    }
    now += utc_offset;                                      // shift to local time
    gmtime_r(&now, out);                                    // split into year/month/day/hour/minute
    return true;
}

void clock_ui_show(int utc_offset, bool full)
{
    static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char time_buf[8], date_buf[24];
    const char *ampm = "";                                  // stays empty in 24-hour mode
    struct tm t;

    if (local_time(utc_offset, &t)) {
#if CLOCK_24H
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t.tm_hour, t.tm_min);
#else
        int hour12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;  // 0 -> 12, 13 -> 1, ...
        snprintf(time_buf, sizeof(time_buf), "%d:%02d", hour12, t.tm_min);
        ampm = t.tm_hour < 12 ? "AM" : "PM";
#endif
        snprintf(date_buf, sizeof(date_buf), "%s %s %d", days[t.tm_wday], months[t.tm_mon], t.tm_mday);
    } else {
        snprintf(time_buf, sizeof(time_buf), "--:--");      // internet time not synced yet
        snprintf(date_buf, sizeof(date_buf), "Waiting for time");
    }

    if (full) {
        epd_init();                                         // wake + fully reset the panel
    }
    epd_clear(EPD_WHITE);

    // Big time, with AM/PM to its right; the pair is centered as one group
    int time_w = epd_text_width(time_buf, TIME_SCALE);
    int ampm_w = ampm[0] ? 4 + epd_text_width(ampm, 2) : 0; // 4 px gap + 2x text
    int x = (EPD_WIDTH - time_w - ampm_w) / 2;
    epd_draw_text(x, TIME_Y, time_buf, TIME_SCALE, EPD_BLACK);
    if (ampm[0]) {
        epd_draw_text(x + time_w + 4, TIME_Y, ampm, 2, EPD_BLACK);  // top-aligned with the digits
    }

    // Date centered underneath, e.g. "Tuesday Oct 27"
    epd_draw_text((EPD_WIDTH - epd_text_width(date_buf, 2)) / 2, DATE_Y, date_buf, 2, EPD_BLACK);

    if (full) {
        epd_display();                                      // full refresh (~2 s, flashes)
    } else {
        epd_display_partial();                              // quick refresh, only changed digits update
    }
}

TickType_t clock_ui_ticks_to_next_minute(void)
{
    int sec = (int)(time(NULL) % 60);                       // seconds into the current minute
    return pdMS_TO_TICKS((60 - sec) * 1000 + 200);          // +200 ms so we land just after the change
}

bool clock_ui_top_of_hour(int utc_offset)
{
    struct tm t;
    return local_time(utc_offset, &t) && t.tm_min == 0;
}

void clock_ui_draw_app_icon(int cx, int cy)
{
    epd_fill_circle(cx, cy, 11, EPD_BLACK);                 // clock face: black disc...
    epd_fill_circle(cx, cy, 9, EPD_WHITE);                  // ...hollowed out, leaving a 2 px ring
    for (int i = 0; i < 2; i++) {                           // hands drawn 2 px thick
        epd_draw_line(cx + i, cy, cx + i, cy - 7, EPD_BLACK);  // minute hand pointing at 12
        epd_draw_line(cx, cy + i, cx + 5, cy + i, EPD_BLACK);  // hour hand pointing at 3
    }
    epd_fill_circle(cx, cy, 1, EPD_BLACK);                  // center pin
}
