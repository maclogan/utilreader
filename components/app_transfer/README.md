# app_transfer: the Transfer app

Runs the web interface (`components/webui`) **only while this app is open**, so the board
isn't serving anything on the network the rest of the time. From a browser you can then
add and delete books and see the saved words.

| File | What it does |
|---|---|
| `transfer_app.c`, `include/transfer_app.h` | The `app_t` and its screen |

## Using it

1. Open **Transfer** from the main menu. The screen shows the address, e.g.
   `http://192.168.1.174`; open it in a browser on the same Wi-Fi.
2. First time in that browser: click **Show code on the board**. The board shows a
   6-digit code; type it into the page. The browser stays paired until the board restarts.
3. Upload, delete, browse saved words. The screen shows upload progress (every 10%) and
   the last thing that happened (`Received: 三体`, `Deleted: ...`).
4. **BACK** (or HOME) stops the server. If a pairing code is showing, the first BACK just
   cancels the code.

The board never reopens Transfer by itself after a power-on (it shows the main menu
instead), so the server only runs when someone opened it on purpose.

## How it works

- `open()` calls `webui_start()`; `close()` (called by main when the app is left any way)
  calls `webui_stop()`.
- The web server runs in its own task and can't draw. When something changes (a code is
  requested, a browser pairs, an upload passes another 10%, a book arrives or is deleted)
  it records it and posts `BUTTON_WAKE`, which reaches this app's `button()`. The app reads
  `webui_status()` and redraws if anything differs from what's on screen: a full refresh
  when switching to or from the code screen, a partial one otherwise.
- While a code shows, `next_wakeup()` is 1 s so the screen notices when the code expires
  (3 minutes). `background()` (every ~30 s) redraws if the Wi-Fi address changed.
