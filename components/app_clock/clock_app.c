/*
 * Clock app logic: redraw when the minute changes (partial refresh), with a full refresh
 * on the hour to clear ghosting, and straight away if the time zone changes (the first
 * forecast after power-on can correct it).
 */
#include "clock_app.h"

#include "clock_ui.h"                   // drawing
#include "local_time.h"                 // local_time_offset()

static TickType_t next_redraw = 0;      // start of the next minute
static int drawn_offset = 0;            // time zone the screen was drawn with

static void draw(bool full)
{
    drawn_offset = local_time_offset();
    clock_ui_show(drawn_offset, full);
    next_redraw = xTaskGetTickCount() + clock_ui_ticks_to_next_minute();
}

static void clock_open(void)
{
    draw(true);
}

static bool clock_button(button_event_t event)
{
    return event != BUTTON_BACK;                            // no controls; BACK leaves
}

static TickType_t clock_next_wakeup(void)
{
    if (local_time_offset() != drawn_offset) {
        return 0;                                           // time zone just changed: redraw now
    }
    return app_ticks_until(next_redraw);
}

static void clock_tick(void)
{
    if (local_time_offset() != drawn_offset) {
        draw(true);
    } else if (app_due(next_redraw)) {
        // New minute: quick partial refresh, but a full one on the hour to clear ghosting
        draw(clock_ui_top_of_hour(drawn_offset));
    }
}

const app_t clock_app = {
    .name = "Clock",
    .draw_icon = clock_ui_draw_app_icon,
    .open = clock_open,
    .button = clock_button,
    .next_wakeup = clock_next_wakeup,
    .tick = clock_tick,
};
