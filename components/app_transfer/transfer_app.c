/*
 * Transfer app: web server on while open, off when closed.
 *
 *  +--------------------------------------+     while a browser pairs:
 *  | Transfer                     running |     +--------------------------------------+
 *  |--------------------------------------|     | Pairing code                         |
 *  | Open in a browser on this Wi-Fi:     |     |                                      |
 *  |  http://192.168.1.174                |     |            123 456                   |
 *  |                                      |     |                                      |
 *  | Received: 三体                        |     | Type it on the web page              |
 *  | BACK: stop sharing                   |     | BACK: cancel                         |
 *  +--------------------------------------+     +--------------------------------------+
 */
#include "transfer_app.h"

#include <stdio.h>                      // snprintf()
#include <string.h>
#include "buttons.h"                    // buttons_post()
#include "epd.h"                  // drawing
#include "podcasts.h"                   // the podcast updater runs while Transfer is open
#include "font16.h"                     // activity text can be Chinese (book titles)
#include "webui.h"                      // the web server
#include "wifi.h"                       // wifi_ip_string()

#define MARGIN 4

static webui_status_t shown;            // what's on screen, to redraw only on changes
static char shown_ip[20];
static char shown_pod[96];

// The podcast updater runs only while Transfer is open (Wi-Fi is busy then anyway, and no
// downloads compete with playback otherwise). Its progress, one line.
static void podcast_line(char *out, size_t size)
{
    pod_update_status_t u;
    podcasts_update_status(&u);
    if (u.running && u.downloading > 0) {
        if (u.percent >= 0) {
            snprintf(out, size, "Podcasts: downloading %d%% (%d left)", u.percent, u.downloading);
        } else {
            snprintf(out, size, "Podcasts: downloading (%d left)", u.downloading);
        }
    } else if (u.running && u.feeds) {
        snprintf(out, size, "Podcasts: checking feed %d of %d", u.feed, u.feeds);
    } else if (u.running) {
        snprintf(out, size, "Podcasts: updating");
    } else if (u.offline) {
        snprintf(out, size, "Podcasts: no Wi-Fi");
    } else if (u.message[0]) {
        snprintf(out, size, "Podcasts: %.70s", u.message);
    } else {
        snprintf(out, size, "Podcasts: up to date");
    }
}

static void wake_main_loop(void)
{
    buttons_post(BUTTON_WAKE);                              // from the updater's task
}

static void centered(int y, const char *text, int scale)
{
    epd_draw_text((EPD_WIDTH - epd_text_width(text, scale)) / 2, y, text, scale, EPD_BLACK);
}

static void draw(bool full)
{
    webui_status_t st;
    webui_status(&st);
    char ip[20] = "";
    bool online = wifi_ip_string(ip, sizeof(ip));
    shown = st;
    memcpy(shown_ip, ip, sizeof(shown_ip));
    podcast_line(shown_pod, sizeof(shown_pod));

    if (!full && !epd_is_awake()) {
        full = true;
    }
    if (full) {
        epd_init();
    }
    epd_clear(EPD_WHITE);

    if (st.code_live) {                                     // pairing: the code, big
        char spaced[8];
        snprintf(spaced, sizeof(spaced), "%.3s %.3s", st.code, st.code + 3);
        font16_draw_text(MARGIN, 0, "Pairing code", EPD_BLACK);
        epd_draw_hline(0, 17, EPD_WIDTH, EPD_BLACK);
        centered(34, spaced, 5);
        centered(84, "Type this code on the web page", 1);
        centered(108, "BACK: cancel", 1);
    } else {
        font16_draw_text(MARGIN, 0, "Transfer", EPD_BLACK);
        const char *state = st.running ? "running" : "stopped";
        epd_draw_text(EPD_WIDTH - MARGIN - epd_text_width(state, 1), 5, state, 1, EPD_BLACK);
        epd_draw_hline(0, 17, EPD_WIDTH, EPD_BLACK);

        if (!online) {
            centered(40, "Connecting to Wi-Fi...", 1);
        } else {
            epd_draw_text(MARGIN, 24, "Open in a browser on this Wi-Fi:", 1, EPD_BLACK);
            char url[32];
            snprintf(url, sizeof(url), "http://%s", ip);
            int scale = epd_text_width(url, 2) <= EPD_WIDTH - 2 * MARGIN ? 2 : 1;
            centered(40, url, scale);
        }

        // Last activity: upload progress bar, or a message (book titles may be Chinese)
        if (st.upload_percent >= 0) {
            char text[32];
            snprintf(text, sizeof(text), "Receiving a book  %d%%", st.upload_percent);
            epd_draw_text(MARGIN, 68, text, 1, EPD_BLACK);
            epd_draw_rect(MARGIN, 80, EPD_WIDTH - 2 * MARGIN, 8, EPD_BLACK, false);
            epd_draw_rect(MARGIN + 2, 82, (EPD_WIDTH - 2 * MARGIN - 4) * st.upload_percent / 100, 4, EPD_BLACK, true);
        } else if (st.activity[0]) {
            const char *p = st.activity;
            int x = MARGIN;
            uint32_t cp;
            while ((cp = font16_next_char(&p)) != 0 && x + font16_char_width(cp) <= EPD_WIDTH - MARGIN) {
                x += font16_draw_char(x, 68, cp, 1, EPD_BLACK);
            }
        } else {
            epd_draw_text(MARGIN, 72, "Waiting for a browser", 1, EPD_BLACK);
        }
        epd_draw_text(MARGIN, 94, shown_pod, 1, EPD_BLACK);
        centered(112, "BACK: stop sharing", 1);
    }

    if (full) {
        epd_display();
    } else {
        epd_display_partial();
    }
}

static void transfer_open(const app_t *self)
{
    webui_start();
    podcasts_update_start(true, wake_main_loop);           // check feeds, download episodes
    draw(true);
}

static bool transfer_button(const app_t *self, button_event_t event)
{
    if (event == BUTTON_BACK) {
        webui_status_t st;
        webui_status(&st);
        if (st.code_live) {
            webui_pairing_cancel();                         // first BACK just cancels the code
            draw(false);
            return true;
        }
        return false;                                       // leave: main calls transfer_close()
    }
    if (event == BUTTON_WAKE) {                             // the web server reports a change
        webui_status_t st;
        webui_status(&st);
        bool code_changed = st.code_live != shown.code_live || strcmp(st.code, shown.code) != 0;
        char pod[96];
        podcast_line(pod, sizeof(pod));
        if (code_changed || st.upload_percent != shown.upload_percent
            || strcmp(st.activity, shown.activity) != 0 || strcmp(pod, shown_pod) != 0) {
            draw(code_changed);                             // the code screen is a whole new screen
        }
    }
    return true;
}

static TickType_t transfer_next_wakeup(const app_t *self)
{
    return shown.code_live ? pdMS_TO_TICKS(1000) : portMAX_DELAY;   // notice the code expiring
}

static void transfer_tick(const app_t *self)
{
    webui_status_t st;
    webui_status(&st);
    char ip[20] = "";
    wifi_ip_string(ip, sizeof(ip));
    if (st.code_live != shown.code_live || strcmp(ip, shown_ip) != 0) {
        draw(true);                                         // code expired, or Wi-Fi came / went
    }
}

static void transfer_background(const app_t *self)
{
    if (app_is_showing(&transfer_app)) {
        transfer_tick(self);                                // every ~30 s: catch Wi-Fi changes
    }
}

static void transfer_close(const app_t *self)
{
    podcasts_update_stop();                                 // a partial download resumes next time
    webui_stop();
}

// Menu icon: two arrows, up and down
static void transfer_icon(const app_t *self, int cx, int cy)
{
    for (int i = 0; i < 2; i++) {
        int x = cx - 7 + i * 14;
        epd_draw_rect(x - 1, cy - 12, 3, 24, EPD_BLACK, true);     // shaft
        int tip = i == 0 ? cy - 14 : cy + 14, dir = i == 0 ? 1 : -1;
        for (int k = 0; k < 6; k++) {                              // arrowhead
            epd_draw_hline(x - k, tip + dir * k, 2 * k + 1, EPD_BLACK);
        }
    }
}

const app_t transfer_app = {
    .id = "transfer",
    .name = "Transfer",
    .draw_icon = transfer_icon,
    .open = transfer_open,
    .button = transfer_button,
    .next_wakeup = transfer_next_wakeup,
    .tick = transfer_tick,
    .background = transfer_background,
    .close = transfer_close,
    .no_resume = true,                  // the web server only runs when opened on purpose
};
