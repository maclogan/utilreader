#include "ui_sidebar.h"

#include <stdio.h>                      // snprintf()
#include "font16.h"                     // titles can be Chinese

static int battery_pct = -1;
static ui_battery_t battery_st = UI_BATTERY_NONE;

void ui_sidebar_set_battery(int percent, ui_battery_t state)
{
    battery_pct = percent;
    battery_st = state;
}

// Length along the bar that ui_sidebar_battery() would use
static int battery_width(bool with_text)
{
    if (battery_pct < 0 || battery_st == UI_BATTERY_NONE) {
        return 0;
    }
    int w = 16;                                             // body + nub
    if (with_text) {
        char text[16];
        snprintf(text, sizeof(text), battery_st == UI_BATTERY_FULL ? "Full" : "%d%%", battery_pct);
        w += 3 + epd_text_width(text, 1);
    }
    return w + (battery_st == UI_BATTERY_CHARGING ? 8 : 0);
}

int ui_sidebar_battery(int end, bool with_text)
{
    if (battery_pct < 0 || battery_st == UI_BATTERY_NONE) {
        return end;
    }
    const int w = 14, h = 8, v = 4;                         // battery body, 4 px in from the edge
    int u = end - w - 2;                                    // leave room for the terminal nub
    epd_draw_rect(u, v, w, h, EPD_BLACK, false);            // outline
    epd_draw_rect(u + w, v + 2, 2, h - 4, EPD_BLACK, true); // terminal nub, towards the top
    int fill = (w - 4) * battery_pct / 100;
    if (fill > 0) {
        epd_draw_rect(u + 2, v + 2, fill, h - 4, EPD_BLACK, true);
    }
    int start = u;
    if (with_text) {
        char text[16];
        snprintf(text, sizeof(text), battery_st == UI_BATTERY_FULL ? "Full" : "%d%%", battery_pct);
        start = u - 3 - epd_text_width(text, 1);
        epd_draw_text(start, v, text, 1, EPD_BLACK);
    }
    if (battery_st == UI_BATTERY_CHARGING) {               // lightning bolt
        int b = start - 8;
        epd_draw_line(b + 4, v - 1, b + 1, v + 4, EPD_BLACK);
        epd_draw_line(b + 1, v + 4, b + 5, v + 4, EPD_BLACK);
        epd_draw_line(b + 5, v + 4, b + 2, v + 9, EPD_BLACK);
        start = b;
    }
    return start;
}

void ui_sidebar_begin(void)
{
    epd_vertical_begin(0, EPD_HEIGHT - 1 - 4);              // drawing x = 0 is 4 px above the bottom
}

void ui_sidebar_rule(void)
{
    epd_draw_rect(UI_SIDEBAR_W - 1, 0, 1, EPD_HEIGHT, EPD_BLACK, true);
}

void ui_sidebar(const char *title, const char *counter)
{
    ui_sidebar_begin();
    // Battery at the top end, with its percentage if the title still fits beside it
    int title_w = title ? font16_text_width(title) : 0;
    int counter_w = counter ? epd_text_width(counter, 1) + 6 : 0;
    bool with_text = title_w + counter_w + battery_width(true) + 6 <= UI_SIDEBAR_LEN;
    int end = ui_sidebar_battery(UI_SIDEBAR_LEN, with_text);   // the title stops before the counter
    if (end < UI_SIDEBAR_LEN) {
        end -= 6;
    }
    if (counter) {
        int w = epd_text_width(counter, 1);
        epd_draw_text(end - w, 5, counter, 1, EPD_BLACK);
        end -= w + 6;
    }
    int x = 0;
    uint32_t cp;
    while (title && (cp = font16_next_char(&title)) != 0 && x + font16_char_width(cp) <= end) {
        x += font16_draw_char(x, 0, cp, 1, EPD_BLACK);
    }
    epd_vertical_end();
    ui_sidebar_rule();
}
