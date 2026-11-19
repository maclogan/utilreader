# main: the launcher

Start-up, the main menu and the event loop. This is the only code that knows which apps
exist; it knows nothing about what happens inside them.

| File | What it does |
|---|---|
| `main.c` | Start-up sequence, the `apps[]` list, the event loop |
| `menu_ui.c/.h` | Draws the row of app tiles (scrolls when there are more than 3) |
| `Kconfig.projbuild` | The *Weather Display* and *Chinese Story* menus in `idf.py menuconfig` |
| `CMakeLists.txt` | Which components the launcher uses |

## Start-up (`app_main`)

1. `board_led_off()`; the display wiring report if *menuconfig → Hardware → display
   diagnostic* is on; `nvs_flash_init()`: settings flash (also needed by Wi-Fi).
2. Stop with a message if no Wi-Fi SSID is configured.
3. `local_time_init()` loads the remembered time zone, `storage_init()` mounts `/store`,
   `worker_init()` starts the background task.
4. Each app's `init()` (e.g. the story app loads today's story from flash).
5. `buttons_init()`, `battery_init()` (finds the fuel gauge, if fitted), then `wifi_connect()` (waits up to 20 s, keeps retrying after that).
6. Internet time via SNTP (waits up to 10 s for the first sync).
7. Every app's `background()` once, so downloads start immediately (the weather app
   fetches the forecast, which also provides the time zone).
8. Back to where it was: the app in the `last_app` setting, or the menu if `on_menu` is
   set (a new board starts on the menu). Transfer is never reopened automatically, so the
   web server only runs when opened on purpose.

## The event loop

Each pass:

```
worker_dispatch()                    apply finished background jobs (their done() callbacks)
wait = min( time to next background run (30 s),
            current app's next_wakeup(),
            100 ms if any job is still running )      <- so results are picked up quickly
buttons_wait(wait)
  timeout  -> every 30 s: a battery reading (redraws the menu if it changed and the menu
              is showing), each app's background(); then current app's tick()
  HOME     -> the current app's close(), then the menu (from anywhere)
  WAKE     -> the current app's button() (another task wants attention, e.g. the web server)
  other    -> current app's button(); if it returns false: its close(), then the menu
              (on the menu itself: wheel up/down = LEFT/RIGHT move, press = SELECT opens)
```

Everything here runs in one task (the "main task"), and so do all app callbacks, so app
state is only ever touched from one place. The only other tasks are the button scanner
(`components/buttons`) and the worker (`components/app_core/worker.c`), and both talk to
the main task through queues.

A screen refresh blocks the loop for its duration (~0.3 s partial, ~2 s full). Button
presses made meanwhile queue up (up to 8) and are handled right after.

## Adding an app

1. **Create the component** `components/app_<name>/`:

   ```
   components/app_notes/
     CMakeLists.txt
     include/notes_app.h      extern const app_t notes_app;
     notes_app.c              the app_t and its callbacks
     notes_ui.c/.h            drawing (optional, but keeps logic and layout apart)
     README.md
   ```

   `CMakeLists.txt`:

   ```cmake
   idf_component_register(SRCS "notes_app.c" "notes_ui.c"
                          INCLUDE_DIRS "include"
                          PRIV_INCLUDE_DIRS "."
                          REQUIRES app_core            # app.h (public header uses it)
                          PRIV_REQUIRES display)       # anything only the .c files use
   ```

2. **Implement `app_t`** (see `components/app_core/README.md` for every callback):

   ```c
   #include "notes_app.h"
   #include "epd_2in13.h"

   static void notes_icon(int cx, int cy) { epd_fill_circle(cx, cy, 10, EPD_BLACK); }

   static void notes_open(void)
   {
       epd_init();                                 // full refresh for the first screen
       epd_clear(EPD_WHITE);
       epd_draw_text(10, 50, "Hello", 2, EPD_BLACK);
       epd_display();
   }

   static bool notes_button(button_event_t e)
   {
       return e != BUTTON_BACK;                    // BACK leaves; nothing else to do yet
   }

   const app_t notes_app = {
       .name = "Notes",
       .draw_icon = notes_icon,
       .open = notes_open,
       .button = notes_button,
   };
   ```

3. **Register it** in two places:
   - `main/CMakeLists.txt`: add `app_notes` to `PRIV_REQUIRES`.
   - `main/main.c`: `#include "notes_app.h"` and add `&notes_app` to `apps[]`.

Order in `apps[]` is menu order. The `last_app` setting stores an index into `apps[]`,
so reordering the list makes the board open a different app at the next power-on (once).

## Menu layout (`menu_ui.c`)

Tiles are 64×64 px with a 13 px label strip; up to three fit across the 250 px screen.
With more apps the row scrolls to keep the highlighted tile visible, and `<` / `>`
arrows show there are more to either side. The battery level (`components/battery`) sits
in the top-right corner above the tiles when a fuel gauge is fitted. An app's icon is drawn by its `draw_icon`,
centered in the area above the label (about 50×50 px).
