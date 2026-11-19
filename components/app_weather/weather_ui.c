#include "weather_ui.h"

#include <math.h>                       // cosf()/sinf() for the sun's rays
#include <stdio.h>                      // snprintf()
#include <time.h>                       // time(), gmtime_r() for the clock
#include "epd_2in13.h"                  // drawing primitives + display refresh
#include "sdkconfig.h"                  // CONFIG_WEATHER_UNITS_* from menuconfig

// Wind unit label, matching the units requested from the API. Kept short to fit.
#if CONFIG_WEATHER_UNITS_METRIC
#define WIND_UNIT "kmh"
#else
#define WIND_UNIT "mph"
#endif

// Layout (pixels). The screen is 250 wide x 122 tall.
//   x 0..74    : "today" panel (30% of the width)
//   x 75       : solid divider line
//   x 76..249  : 3 columns x 2 rows of day cells for the next 6 days
#define PANEL_W       75                // width of the today panel
#define PANEL_CX      (PANEL_W / 2)     // horizontal center of the today panel
#define GRID_X0       76                // left edge of the day grid
#define GRID_COLS     3                 // day cells per row
#define CELL_W        58                // 3 * 58 = 174 px = the rest of the width
#define CELL_H        61                // 2 * 61 = 122 px = full height

// Simplified icon categories that the WMO weather codes get grouped into
typedef enum { ICON_SUN, ICON_PARTLY, ICON_CLOUD, ICON_FOG, ICON_RAIN, ICON_SNOW, ICON_STORM } icon_t;

// Maps a WMO weather code (https://open-meteo.com/en/docs, "WMO Weather interpretation codes")
// to one of our icons
static icon_t icon_for_code(int code)
{
    if (code == 0) return ICON_SUN;                         // clear sky
    if (code <= 2) return ICON_PARTLY;                      // mainly clear / partly cloudy
    if (code == 3) return ICON_CLOUD;                       // overcast
    if (code == 45 || code == 48) return ICON_FOG;          // fog / rime fog
    if (code >= 51 && code <= 67) return ICON_RAIN;         // drizzle, rain, freezing rain
    if (code >= 71 && code <= 77) return ICON_SNOW;         // snowfall, snow grains
    if (code >= 80 && code <= 82) return ICON_RAIN;         // rain showers
    if (code == 85 || code == 86) return ICON_SNOW;         // snow showers
    if (code >= 95) return ICON_STORM;                      // thunderstorms
    return ICON_CLOUD;                                      // anything unexpected
}

// Day of week for a date: 0 = Sunday ... 6 = Saturday (Sakamoto's algorithm)
static int day_of_week(int y, int m, int d)
{
    static const int offset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};  // per-month correction
    if (m < 3) {
        y -= 1;                                             // Jan/Feb count as end of previous year
    }
    return (y + y / 4 - y / 100 + y / 400 + offset[m - 1] + d) % 7;
}

// Draws text horizontally centered on cx
static void text_centered(int cx, int y, const char *text, int scale)
{
    epd_draw_text(cx - epd_text_width(text, scale) / 2, y, text, scale, EPD_BLACK);
}

/* ---------- icon drawing (each fits in roughly 24x24 around cx, cy) ---------- */

// Filled sun with 8 rays. r = disc radius.
static void draw_sun(int cx, int cy, int r)
{
    epd_fill_circle(cx, cy, r, EPD_BLACK);                  // the disc
    for (int i = 0; i < 8; i++) {                           // 8 rays, 45 degrees apart
        float a = i * (float)M_PI / 4;                      // angle of this ray in radians
        int x0 = cx + (int)lroundf(cosf(a) * (r + 3));      // ray starts a little outside the disc
        int y0 = cy + (int)lroundf(sinf(a) * (r + 3));
        int x1 = cx + (int)lroundf(cosf(a) * (r + 5));      // ...and is 2-3 px long
        int y1 = cy + (int)lroundf(sinf(a) * (r + 5));
        epd_draw_line(x0, y0, x1, y1, EPD_BLACK);
    }
}

// Outlined cloud built from three overlapping circles on a flat base. (cx, cy) = cloud center.
static void draw_cloud(int cx, int cy)
{
    // Pass 1: the full cloud shape in black
    epd_fill_circle(cx - 5, cy + 1, 5, EPD_BLACK);          // left puff
    epd_fill_circle(cx + 2, cy - 2, 6, EPD_BLACK);          // big middle puff
    epd_fill_circle(cx + 7, cy + 2, 4, EPD_BLACK);          // right puff
    epd_draw_rect(cx - 5, cy + 1, 13, 6, EPD_BLACK, true);  // flat bottom joining the puffs
    // Pass 2: the same shape 2 px smaller in white, leaving a 2 px black outline
    epd_fill_circle(cx - 5, cy + 1, 3, EPD_WHITE);
    epd_fill_circle(cx + 2, cy - 2, 4, EPD_WHITE);
    epd_fill_circle(cx + 7, cy + 2, 2, EPD_WHITE);
    epd_draw_rect(cx - 5, cy + 1, 13, 4, EPD_WHITE, true);  // stops 2 px above the bottom edge
}

// Thick (2 px) line, for rain streaks and lightning so they read at a distance
static void thick_line(int x0, int y0, int x1, int y1)
{
    epd_draw_line(x0, y0, x1, y1, EPD_BLACK);
    epd_draw_line(x0 + 1, y0, x1 + 1, y1, EPD_BLACK);       // second line 1 px to the right
}

static void draw_icon(int cx, int cy, icon_t icon)
{
    switch (icon) {
    case ICON_SUN:
        draw_sun(cx, cy, 6);                                // big sun centered in the slot
        break;
    case ICON_PARTLY:
        draw_sun(cx - 5, cy - 4, 4);                        // small sun up and to the left...
        draw_cloud(cx + 1, cy + 4);                         // ...partly hidden behind a cloud
        break;
    case ICON_CLOUD:
        draw_cloud(cx - 1, cy);                             // just a cloud
        break;
    case ICON_FOG:
        for (int i = 0; i < 4; i++) {                       // four staggered horizontal bars
            int shift = (i % 2) ? 2 : -2;                   // alternate left/right offset
            epd_draw_rect(cx - 9 + shift, cy - 7 + i * 5, 18, 2, EPD_BLACK, true);
        }
        break;
    case ICON_RAIN:
        draw_cloud(cx - 1, cy - 5);                         // cloud raised to make room below
        for (int i = -1; i <= 1; i++) {                     // three slanted rain streaks
            thick_line(cx + i * 6 + 1, cy + 5, cx + i * 6 - 2, cy + 11);
        }
        break;
    case ICON_SNOW:
        draw_cloud(cx - 1, cy - 5);
        for (int i = -1; i <= 1; i++) {                     // three small snowflakes ("+" shapes)
            int fx = cx + i * 6;                            // flake x
            int fy = cy + (i == 0 ? 10 : 7);                // middle flake sits lower
            epd_draw_hline(fx - 2, fy, 5, EPD_BLACK);       // horizontal arm
            epd_draw_line(fx, fy - 2, fx, fy + 2, EPD_BLACK);  // vertical arm
        }
        break;
    case ICON_STORM:
        draw_cloud(cx - 1, cy - 5);
        thick_line(cx + 1, cy + 3, cx - 3, cy + 8);         // lightning bolt: down-left...
        thick_line(cx - 3, cy + 8, cx + 1, cy + 8);         // ...across...
        thick_line(cx + 1, cy + 8, cx - 3, cy + 13);        // ...and down-left again
        break;
    }
}

// Bold 1 px line drawn 3 times (right and down copies) so it reads well in any direction
static void bold_line(int x0, int y0, int x1, int y1)
{
    epd_draw_line(x0, y0, x1, y1, EPD_BLACK);
    epd_draw_line(x0 + 1, y0, x1 + 1, y1, EPD_BLACK);
    epd_draw_line(x0, y0 + 1, x1, y1 + 1, EPD_BLACK);
}

// Arrow centered on (cx, cy) pointing the way the wind is blowing, with up = north.
// from_deg is the meteorological direction (where the wind comes FROM), e.g. 0 = from the north.
// len = how far the arrow reaches from its center each way.
static void draw_wind_arrow(int cx, int cy, int from_deg, int len)
{
    float to = (from_deg + 180) * (float)M_PI / 180;        // flip 180: direction the air moves TOWARD
    float dx = sinf(to);                                    // compass -> screen: east is +x...
    float dy = -cosf(to);                                   // ...north is -y (up)
    int tip_x = cx + (int)lroundf(dx * len);                // arrow head end
    int tip_y = cy + (int)lroundf(dy * len);
    int tail_x = cx - (int)lroundf(dx * len);               // opposite end
    int tail_y = cy - (int)lroundf(dy * len);
    bold_line(tail_x, tail_y, tip_x, tip_y);                // the shaft

    for (int side = -1; side <= 1; side += 2) {             // the two barbs of the arrow head
        float a = to + (float)M_PI + side * 0.6f;           // point back from the tip, ~35 deg off the shaft
        int bx = tip_x + (int)lroundf(sinf(a) * 4);         // barb end, 4 px from the tip
        int by = tip_y - (int)lroundf(cosf(a) * 4);
        bold_line(tip_x, tip_y, bx, by);
    }
}

// Tiny 5x7 raindrop drawn with its top-left corner at (x, y)
static void draw_droplet(int x, int y)
{
    epd_fill_circle(x + 2, y + 5, 2, EPD_BLACK);            // round bottom
    epd_draw_line(x + 2, y + 1, x + 1, y + 3, EPD_BLACK);   // left side of the point
    epd_draw_line(x + 2, y + 1, x + 3, y + 3, EPD_BLACK);   // right side of the point
    epd_draw_pixel(x + 2, y + 2, EPD_BLACK);                // fill the middle of the point
}

// Small degree symbol (a 5x5 ring) with its top-left corner at (x, y)
static void draw_degree(int x, int y)
{
    epd_fill_circle(x + 2, y + 2, 2, EPD_BLACK);            // solid dot...
    epd_draw_pixel(x + 2, y + 2, EPD_WHITE);                // ...with the center punched out
}

// Dotted horizontal line (every other pixel), used to separate areas lightly
static void dotted_hline(int x, int y, int w)
{
    for (int i = 0; i < w; i += 2) {
        epd_draw_pixel(x + i, y, EPD_BLACK);
    }
}

// Dotted vertical line (every other pixel)
static void dotted_vline(int x, int y, int h)
{
    for (int i = 0; i < h; i += 2) {
        epd_draw_pixel(x, y + i, EPD_BLACK);
    }
}

// Draws a number centered on cx at 2x size, dropping to 1x if it's too wide for max_w
static void number_centered(int cx, int y, int value, int max_w)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    int scale = epd_text_width(buf, 2) <= max_w ? 2 : 1;    // e.g. "-12" is too wide at 2x
    text_centered(cx, y + (scale == 2 ? 0 : 4), buf, scale);  // 1x text is nudged down to stay centered
}

// Label strip at the top of a day area. Highlighted = black strip with white text
// (shows which day LEFT/RIGHT have selected); otherwise plain black text.
static void day_label(int x, int y, int w, const char *text, bool highlighted)
{
    int tx = x + w / 2 - epd_text_width(text, 1) / 2;       // centered in the strip
    if (highlighted) {
        epd_draw_rect(x, y, w, 11, EPD_BLACK, true);        // 11 px tall black strip
        epd_draw_text(tx, y + 2, text, 1, EPD_WHITE);
    } else {
        epd_draw_text(tx, y + 2, text, 1, EPD_BLACK);
    }
}

/* ---------- screen areas ---------- */

// Left 30% of the screen: today's forecast plus current conditions
static void draw_today_panel(const weather_t *w, bool highlighted)
{
    const weather_day_t *today = &w->days[0];
    char buf[24];

    // "Today 10/26" across the top
    snprintf(buf, sizeof(buf), "Today %02d/%02d", today->month, today->day);
    day_label(0, 1, PANEL_W, buf, highlighted);

    // Icon for the weather right now, on the left
    draw_icon(15, 28, icon_for_code(w->current_code));

    // Current temperature, big (3x) to the right of the icon; 2x if it has 3 characters
    snprintf(buf, sizeof(buf), "%d", w->current_temp);
    int scale = epd_text_width(buf, 3) <= 36 ? 3 : 2;       // keep it inside the 75 px panel
    int ty = (scale == 3) ? 18 : 22;                        // 2x text is shorter, so move it down a bit
    int end_x = epd_draw_text(31, ty, buf, scale, EPD_BLACK);  // returns x just past the text
    draw_degree(end_x, ty);                                 // degree mark at the top-right of the number

    // Today's high and low, e.g. "Hi 73 Lo 43"
    snprintf(buf, sizeof(buf), "Hi %d Lo %d", today->temp_high, today->temp_low);
    text_centered(PANEL_CX, 48, buf, 1);

    // Today's rain chance: droplet + "40% rain", centered as one unit
    snprintf(buf, sizeof(buf), "%d%% rain", today->rain_chance);
    int left = PANEL_CX - (7 + epd_text_width(buf, 1)) / 2; // 7 = droplet width + gap
    draw_droplet(left, 61);
    epd_draw_text(left + 7, 61, buf, 1, EPD_BLACK);

    dotted_hline(4, 74, PANEL_W - 8);                       // separates forecast from wind

    // Time of this update in local time, e.g. "As of 17:25".
    // Before internet time is synced it shows dashes.
    time_t now = time(NULL);                                // seconds since 1970, UTC
    if (now > 1700000000) {                                 // clock has been set (later than Nov 2023)
        now += w->utc_offset;                               // shift UTC to the location's local time
        struct tm t;
        gmtime_r(&now, &t);                                 // split into hours/minutes
        snprintf(buf, sizeof(buf), "As of %02d:%02d", t.tm_hour, t.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "As of --:--");
    }
    text_centered(PANEL_CX, 80, buf, 1);

    // Current wind: arrow + big speed + unit
    // The arrow, speed and unit are centered in the panel as one group
    snprintf(buf, sizeof(buf), "%d", w->current_wind);
    int group_w = 14                                        // arrow (11 px) + 3 px gap
                + epd_text_width(buf, 2) + 2                // speed at 2x + gap
                + epd_text_width(WIND_UNIT, 1);             // unit at 1x
    left = PANEL_CX - group_w / 2;                          // left edge of the whole group
    draw_wind_arrow(left + 5, 100, w->current_wind_dir, 5); // which way it's blowing right now
    end_x = epd_draw_text(left + 14, 94, buf, 2, EPD_BLACK);  // speed at 2x
    epd_draw_text(end_x, 101, WIND_UNIT, 1, EPD_BLACK);     // unit at 1x, bottom-aligned with the speed

    // Strongest wind expected today, e.g. "Max 15mph"
    snprintf(buf, sizeof(buf), "Max %d" WIND_UNIT, today->wind_max);
    text_centered(PANEL_CX, 112, buf, 1);
}

// One 58x61 cell of the forecast grid, top-left corner at (x0, y0):
//
//    Tue 10/27          <- day + date
//    [icon]    73       <- icon on the left, high on the right
//              --
//              43       <- low
//    o40%    ^ 12       <- rain chance, wind arrow + speed
static void draw_day_cell(int x0, int y0, const weather_day_t *d, bool highlighted)
{
    static const char *day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    char buf[16];

    // Day name + date, e.g. "Tue 10/27" (CELL_W - 1 keeps the strip off the dotted divider)
    snprintf(buf, sizeof(buf), "%s %02d/%02d",
             day_names[day_of_week(d->year, d->month, d->day)], d->month, d->day);
    day_label(x0, y0 + 1, CELL_W - 1, buf, highlighted);

    draw_icon(x0 + 14, y0 + 27, icon_for_code(d->weather_code));  // weather picture, left half

    // High and low stacked in the right half, with a short bar between them
    number_centered(x0 + 43, y0 + 13, d->temp_high, 28);
    epd_draw_hline(x0 + 39, y0 + 29, 9, EPD_BLACK);
    number_centered(x0 + 43, y0 + 32, d->temp_low, 28);

    // Bottom row, left: droplet + rain chance, e.g. "40%"
    snprintf(buf, sizeof(buf), "%d%%", d->rain_chance);     // "%%" prints a literal '%'
    draw_droplet(x0 + 2, y0 + 51);
    epd_draw_text(x0 + 9, y0 + 51, buf, 1, EPD_BLACK);

    // Bottom row, right: wind arrow + max speed (unit is shown in the today panel)
    draw_wind_arrow(x0 + 40, y0 + 54, d->wind_dir, 4);
    snprintf(buf, sizeof(buf), "%d", d->wind_max);
    epd_draw_text(x0 + 47, y0 + 51, buf, 1, EPD_BLACK);
}

/* ---------- screens ---------- */

// Draws the whole forecast screen into the frame buffer (doesn't touch the panel yet).
// hover = highlighted day (0 = today, 1-6 = grid), or -1 for none.
static void render(const weather_t *w, int hover)
{
    epd_clear(EPD_WHITE);                                   // start from a blank white frame

    draw_today_panel(w, hover == 0);                        // left 30%: today + current conditions

    // Solid line between the today panel and the grid
    epd_draw_rect(PANEL_W, 0, 1, EPD_HEIGHT, EPD_BLACK, true);

    // Light dotted grid lines between the six day cells
    dotted_hline(GRID_X0, CELL_H - 1, EPD_WIDTH - GRID_X0); // between the two rows
    for (int c = 1; c < GRID_COLS; c++) {                   // between the three columns
        dotted_vline(GRID_X0 + c * CELL_W - 1, 0, EPD_HEIGHT);
    }

    // Days 1-6 (tomorrow onward) fill the grid left to right, top row first
    for (int i = 1; i < WEATHER_DAYS; i++) {
        int cell = i - 1;                                   // 0..5
        int x0 = GRID_X0 + (cell % GRID_COLS) * CELL_W;     // column 0, 1, 2
        int y0 = (cell / GRID_COLS) * CELL_H;               // row 0 or 1
        draw_day_cell(x0, y0, &w->days[i], hover == i);
    }
}

void weather_ui_show(const weather_t *w, int hover)
{
    epd_init();                                             // wake + fully reset the panel
    render(w, hover);                                       // draw everything into the frame buffer
    epd_display();                                          // full refresh (~2 s of flashing), clears ghosting
    // No epd_sleep() here: partial refreshes need the panel awake. It idles at
    // very low power anyway, since each update switches its high voltage off when done.
}

void weather_ui_show_partial(const weather_t *w, int hover)
{
    render(w, hover);                                       // redraw everything; only changed pixels will update
    epd_display_partial();                                  // quick refresh (~0.5 s), no flashing
}

/* ---------- day detail screen ---------- */

// Short description of a WMO weather code (max ~17 chars to fit the left column)
static const char *condition_text(int code)
{
    switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "T-storm w/ hail";
    default: return "";
    }
}

// 8-point compass label for a direction in degrees (where the wind comes FROM)
static const char *compass(int deg)
{
    static const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[((deg % 360 + 360 + 22) / 45) % 8];        // +22 so each label covers +/-22.5 deg
}

// Precipitation unit label and format, matching the units requested from the API
#if CONFIG_WEATHER_UNITS_METRIC
#define PRECIP_FMT "%.1fmm"
#else
#define PRECIP_FMT "%.2fin"
#endif

// Detail layout (pixels)
#define DETAIL_COL_W   104              // width of the left (text) column
#define GRAPH_X0       128              // x of hour 0 on the graph
#define GRAPH_STEP     5                // px per hour: 23 steps * 5 = 115 px wide
#define GRAPH_TOP      34               // y of the warmest hour
#define GRAPH_BOTTOM   84               // y of the coldest hour
#define BARS_BOTTOM    104              // bottom of the rain-chance bars
#define BARS_H         14               // a 100% bar is this tall

// Hourly temperature line + rain-chance bars, right side of the detail screen
static void draw_hourly_graph(const weather_t *w, int day)
{
    const weather_day_t *d = &w->days[day];
    char buf[8];

    // Temperature range for scaling the line to the graph height
    int tmin = d->hourly_temp[0], tmax = d->hourly_temp[0];
    for (int h = 1; h < 24; h++) {
        if (d->hourly_temp[h] < tmin) tmin = d->hourly_temp[h];
        if (d->hourly_temp[h] > tmax) tmax = d->hourly_temp[h];
    }
    int range = (tmax - tmin) > 0 ? (tmax - tmin) : 1;      // avoid dividing by 0 on a flat day

    epd_draw_text(EPD_WIDTH - 3 - epd_text_width("Hourly", 1), 23, "Hourly", 1, EPD_BLACK);  // title

    // Axis labels: warmest temperature at the top, coldest at the bottom
    snprintf(buf, sizeof(buf), "%d", tmax);
    epd_draw_text(108, GRAPH_TOP - 3, buf, 1, EPD_BLACK);
    snprintf(buf, sizeof(buf), "%d", tmin);
    epd_draw_text(108, GRAPH_BOTTOM - 4, buf, 1, EPD_BLACK);
    draw_droplet(112, BARS_BOTTOM - 8);                     // marks the row of rain bars

    // Light dotted lines at 6 am, noon and 6 pm
    for (int h = 6; h <= 18; h += 6) {
        dotted_vline(GRAPH_X0 + h * GRAPH_STEP, GRAPH_TOP, BARS_BOTTOM - GRAPH_TOP);
    }

    // Hour labels under the graph
    static const char *labels[] = {"12a", "6a", "12p", "6p"};
    for (int i = 0; i < 4; i++) {
        int x = GRAPH_X0 + i * 6 * GRAPH_STEP;              // 0, 6, 12, 18 hours
        epd_draw_text(x - epd_text_width(labels[i], 1) / 2, BARS_BOTTOM + 4, labels[i], 1, EPD_BLACK);
    }

    // Rain-chance bars, one per hour, growing up from BARS_BOTTOM
    for (int h = 0; h < 24; h++) {
        int bar_h = d->hourly_rain[h] * BARS_H / 100;       // 0-100 % -> 0-14 px
        if (bar_h > 0) {
            epd_draw_rect(GRAPH_X0 + h * GRAPH_STEP - 2, BARS_BOTTOM - bar_h, 4, bar_h, EPD_BLACK, true);
        }
    }
    epd_draw_hline(GRAPH_X0 - 2, BARS_BOTTOM, 23 * GRAPH_STEP + 5, EPD_BLACK);  // baseline for the bars

    // Temperature line: connect each hour to the next
    for (int h = 0; h < 23; h++) {
        int y0 = GRAPH_BOTTOM - (d->hourly_temp[h] - tmin) * (GRAPH_BOTTOM - GRAPH_TOP) / range;
        int y1 = GRAPH_BOTTOM - (d->hourly_temp[h + 1] - tmin) * (GRAPH_BOTTOM - GRAPH_TOP) / range;
        bold_line(GRAPH_X0 + h * GRAPH_STEP, y0, GRAPH_X0 + (h + 1) * GRAPH_STEP, y1);
    }

    // Today only: small triangle above the graph pointing at the current hour
    time_t now = time(NULL);
    if (day == 0 && now > 1700000000) {                     // clock has been set
        int hour = (int)(((now + w->utc_offset) % 86400) / 3600);  // local hour 0-23
        int x = GRAPH_X0 + hour * GRAPH_STEP;
        for (int i = 0; i < 4; i++) {                       // 4 rows, narrowing to a point
            epd_draw_hline(x - 3 + i, GRAPH_TOP - 7 + i, 7 - 2 * i, EPD_BLACK);
        }
    }
}

// Draws the full-screen detail view for one day into the frame buffer
static void render_day(const weather_t *w, int day)
{
    static const char *day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday"};
    const weather_day_t *d = &w->days[day];
    char buf[32];

    epd_clear(EPD_WHITE);

    // Header: "Tuesday 10/27" big on the left, "3/7" (which day of the 7) on the right
    snprintf(buf, sizeof(buf), "%s %02d/%02d",
             day == 0 ? "Today" : day_names[day_of_week(d->year, d->month, d->day)], d->month, d->day);
    epd_draw_text(4, 3, buf, 2, EPD_BLACK);
    snprintf(buf, sizeof(buf), "%d/%d", day + 1, WEATHER_DAYS);
    epd_draw_text(EPD_WIDTH - 3 - epd_text_width(buf, 1), 7, buf, 1, EPD_BLACK);
    epd_draw_hline(0, 20, EPD_WIDTH, EPD_BLACK);            // line under the header
    epd_draw_rect(DETAIL_COL_W + 1, 21, 1, EPD_HEIGHT - 21, EPD_BLACK, true);  // column divider

    // Left column, top: icon, big high, smaller low
    draw_icon(16, 36, icon_for_code(d->weather_code));
    snprintf(buf, sizeof(buf), "%d", d->temp_high);
    int scale = epd_text_width(buf, 3) <= 33 ? 3 : 2;       // 3-character temps drop to 2x
    int x = epd_draw_text(34, scale == 3 ? 25 : 30, buf, scale, EPD_BLACK);
    draw_degree(x, scale == 3 ? 25 : 30);
    snprintf(buf, sizeof(buf), "%d", d->temp_low);          // low, bottom-aligned with the high
    int low_scale = (x + 7 + epd_text_width(buf, 2) <= DETAIL_COL_W) ? 2 : 1;
    epd_draw_text(x + 7, low_scale == 2 ? 32 : 38, buf, low_scale, EPD_BLACK);

    epd_draw_text(3, 52, condition_text(d->weather_code), 1, EPD_BLACK);  // e.g. "Partly cloudy"

    // Left column, stats: one line each
    snprintf(buf, sizeof(buf), "Rain %d%% " PRECIP_FMT, d->rain_chance, d->precip_sum);
    epd_draw_text(3, 63, buf, 1, EPD_BLACK);
    snprintf(buf, sizeof(buf), "Wind %d" WIND_UNIT " %s", d->wind_max, compass(d->wind_dir));
    epd_draw_text(3, 74, buf, 1, EPD_BLACK);
    snprintf(buf, sizeof(buf), "Gust %d" WIND_UNIT " UV %d", d->gust_max, d->uv_max);
    epd_draw_text(3, 85, buf, 1, EPD_BLACK);
    snprintf(buf, sizeof(buf), "Sun %s-%s", d->sunrise, d->sunset);
    epd_draw_text(3, 96, buf, 1, EPD_BLACK);
    snprintf(buf, sizeof(buf), "Feels %d/%d", d->feels_high, d->feels_low);
    epd_draw_text(3, 107, buf, 1, EPD_BLACK);

    draw_hourly_graph(w, day);                              // right side
}

void weather_ui_show_day(const weather_t *w, int day, bool full)
{
    if (full) {
        epd_init();                                         // wake + fully reset the panel
    }
    render_day(w, day);
    if (full) {
        epd_display();                                      // full refresh: whole screen changed
    } else {
        epd_display_partial();                              // quick refresh: stepping between days
    }
}

void weather_ui_draw_app_icon(int cx, int cy)
{
    draw_icon(cx, cy, ICON_PARTLY);                         // reuse the "partly cloudy" forecast icon
}

void weather_ui_show_error(const char *title, const char *detail)
{
    epd_init();
    epd_clear(EPD_WHITE);
    epd_draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK, false);  // border around the screen
    text_centered(EPD_WIDTH / 2, 30, title, 2);             // big title
    text_centered(EPD_WIDTH / 2, 64, detail, 1);            // smaller explanation
    epd_display();
    // No epd_sleep(): waking from deep sleep needs a pulse on RST, and the next screen
    // usually follows within seconds. Like the other screens, the panel idles awake.
}
