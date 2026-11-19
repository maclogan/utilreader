#include "ui_sidebar.h"

#include "font16.h"                     // titles can be Chinese

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
    int end = UI_SIDEBAR_LEN;                               // the title stops before the counter
    if (counter) {
        int w = epd_text_width(counter, 1);
        epd_draw_text(UI_SIDEBAR_LEN - w, 5, counter, 1, EPD_BLACK);
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
