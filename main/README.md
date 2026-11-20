# main: the launcher

Start-up, the main menu and the event loop. This is the only code that knows which apps
exist; it knows nothing about what happens inside them.

| File | What it does |
|---|---|
| `main.c` | Start-up sequence, registering the native apps (Story, Cards, Books, Music, then script apps, then Transfer, Settings, Power), the event loop |
| `menu_ui.c/.h` | Draws the row of app tiles (scrolls when there are more than 4) |
| `Kconfig.projbuild` | The *Wi-Fi*, *Chinese Story* and *Hardware* menus in `idf.py menuconfig` |
| `CMakeLists.txt` | Which components the launcher uses |

## Start-up (`app_main`)

1. `nvs_flash_init()`: settings flash (also needed by Wi-Fi). The saved screen rotation
   (`SETTING_ROTATE_180`) is applied before anything is drawn. Then `power_boot_check()`:
   after a wake-up from "off", carry on only if BACK / HOME is being held (otherwise
   straight back to sleep; see `components/power`). Then the display wiring report if
   *menuconfig → Hardware → display diagnostic* is on.
2. Stop with a message if no Wi-Fi SSID is configured.
3. `local_time_init()` loads the remembered time zone and, after a reset, sets the clock
   from the clock chip; `storage_init()` mounts `/store`,
   `worker_init()` starts the background task.
4. Each app's `init()` (e.g. the story app loads today's story from flash).
5. `buttons_init()`, `battery_init()` (wakes the fuel gauge), then `wifi_connect()` (waits up to 20 s, keeps retrying after that).
6. Internet time via SNTP, re-checked hourly; every sync is copied to the clock chip
   (`on_internet_time()`). Start-up waits up to 10 s for the first sync only if the clock
   isn't set yet.
7. Every app's `background()` once (hidden apps too), so downloads start immediately (the Weather app
   fetches the forecast, which also provides the time zone).
8. Back to where it was: the app whose id is in the `last_app_id` setting, or the menu if `on_menu` is
   set (a new board starts on the menu). Apps with `no_resume` set (Transfer, Power) are
   never reopened automatically, so the web server only runs when opened on purpose.

## The event loop

Each pass:

```
worker_dispatch()                    apply finished background jobs (their done() callbacks)
wait = min( time to next background run (30 s),
            current app's next_wakeup(),
            100 ms if any job is still running )      <- so results are picked up quickly
buttons_wait(wait)
  timeout  -> every 30 s: a battery reading (redraws the menu if it changed and the menu
              is showing), each app's background(); then current app's tick(), or on the
              menu, menu_ui_tick() (the status bar's clock; the wait also covers the next
              minute change while the menu shows)
  LOCK_CHANGED -> redraw the menu's padlock (if the menu is showing); apps never see it
  HOME     -> the current app's close(), then the menu (from anywhere)
  SELECT_HOLD -> for apps with select_hold (Music, script apps), passed on; for the
              menu and other apps it becomes SELECT
  WAKE     -> the current app's button() (another task wants attention, e.g. the web server)
  other    -> current app's button(); if it returns false: its close(), then the menu
              (on the menu itself: LEFT/RIGHT move, SELECT opens)
```

Everything here runs in one task (the "main task"), and so do all app callbacks, so app
state is only ever touched from one place. The only other tasks are the button scanner
(`components/buttons`) and the worker (`components/app_core/worker.c`), and both talk to
the main task through queues.

A screen refresh blocks the loop for its duration (~0.6 s partial, ~3 s full). Button
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
   #include "epd.h"

   static void notes_icon(const app_t *self, int cx, int cy) { epd_fill_circle(cx, cy, 10, EPD_BLACK); }

   static void notes_open(const app_t *self)
   {
       epd_init();                                 // full refresh for the first screen
       epd_clear(EPD_WHITE);
       epd_draw_text(10, 50, "Hello", 2, EPD_BLACK);
       epd_display();
   }

   static bool notes_button(const app_t *self, button_event_t e)
   {
       return e != BUTTON_BACK;                    // BACK leaves; nothing else to do yet
   }

   const app_t notes_app = {
       .id = "notes",                              // stable: remembered as the last app
       .name = "Notes",
       .draw_icon = notes_icon,
       .open = notes_open,
       .button = notes_button,
   };
   ```

3. **Register it** in two places:
   - `main/CMakeLists.txt`: add `app_notes` to `PRIV_REQUIRES`.
   - `main/main.c`: `#include "notes_app.h"` and `app_registry_add(&notes_app, false);` in
     `register_apps()`.

The order of `register_apps()` is the menu order; script apps follow the native ones
(alphabetically), and the system apps (`true`: Transfer, Settings, Power) stay last. The
app to reopen at power-on is remembered by `id`, so the order can change freely.

Apps don't have to be compiled in: a Lua app installed from the web page appears in the
menu straight away. See [docs/api.md](../docs/api.md) and `components/app_script`.

## Menu layout (`menu_ui.c`)

Tiles are 58×64 px with a 13 px label strip; up to four fit right of the status bar.
With more apps the row scrolls to keep the highlighted tile visible, and `<` / `>`
arrows show there are more to either side. The status bar on the left (`ui_sidebar.h`)
reads bottom to top: the time, a padlock while the lock switch is on, and at the top the
battery (`components/battery`) when one is connected. The time updates every minute with
a partial refresh, and a full refresh on the hour clears ghosting. An app's icon is drawn by its `draw_icon`,
centered in the area above the label (about 50×50 px).
