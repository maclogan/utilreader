/*
 * The board's web interface on port 80: manage e-books and see the saved words. It only
 * runs while the Transfer app is open (webui_start / webui_stop). See README.md.
 *
 * Pairing: the API only answers browsers holding a token. To get one, the page asks for a
 * pairing code; the board shows a random 6-digit code on its screen (the Transfer app draws
 * it, from webui_status()), and the code typed into the page is exchanged for a token.
 * Tokens last until the board restarts.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

// What the Transfer app shows
typedef struct {
    bool running;                       // server started
    bool code_live;                     // a pairing code should be on screen...
    char code[7];                       // ...this one ("123456")
    int upload_percent;                 // book upload in progress: 0-99, -1 = none
    char activity[128];                 // last thing that happened, UTF-8 ("Received: 三体"), or ""
} webui_status_t;

// Starts / stops the web server. Stopping also withdraws a pairing code on offer.
esp_err_t webui_start(void);
void webui_stop(void);

// Current status. Whenever it changes the web server wakes the main loop (BUTTON_WAKE).
void webui_status(webui_status_t *out);

// Withdraws the pairing code on screen (e.g. EXIT pressed while it shows)
void webui_pairing_cancel(void);
