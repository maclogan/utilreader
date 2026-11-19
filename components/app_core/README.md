# app_core: what apps are built on

| File | Provides |
|---|---|
| `include/app.h`, `app.c` | `app_t`, the interface every app implements, plus small timing helpers |
| `include/app_registry.h`, `app_registry.c` | The list of apps on the menu (native and script), in menu order |
| `include/worker.h`, `worker.c` | Background task for slow (network) work |
| `include/settings.h`, `settings.c` | Small values remembered across power-offs (NVS) |
| `include/storage.h`, `storage.c` | The `/store` file system (SPIFFS on the `storage` partition) |
| `include/local_time.h`, `local_time.c` | Local time and time zone shared by all apps |

## `app_t`: the app interface

```c
struct app {
    const char *id;                           // stable id, remembered as the last app
    const char *name;                         // menu tile label
    void (*draw_icon)(const app_t *self, int cx, int cy);  // menu tile icon (~32x32 px)
    void (*init)(const app_t *self);          // optional: once at power-on (or install)
    void (*open)(const app_t *self);          // opened from the menu: draw (full refresh)
    bool (*button)(const app_t *self, button_event_t event);  // false = leave the app
    TickType_t (*next_wakeup)(const app_t *self);  // optional: ticks until tick() is wanted
    void (*tick)(const app_t *self);          // optional: scheduled work while on screen
    void (*background)(const app_t *self);    // optional: every ~30 s, on screen or not
    void (*close)(const app_t *self);         // optional: the app is leaving the screen
    bool no_resume;                           // never reopened by itself at power-on
    void *ctx;                                // for the implementation
};
```

Every callback gets the app itself, so one implementation can serve many apps: the Lua
host (`components/app_script`) gives every installed script app an `app_t` with the same
callbacks and finds its state through `self->ctx`. Native apps ignore `self`.

## The app registry

`app_registry_add(app, system)` puts an app on the menu: regular apps in the order added,
system apps (`true`: Transfer, Settings, Power) always at the end. The user can reorder
the regular apps and hide apps from the web portal; the choice is kept in flash
(`menu_order`, `menu_hidden`) and applied by `app_registry_reload_prefs()`. Transfer and
Power can't be hidden. `app_registry_list()` returns the menu (visible apps, in order),
`app_registry_all()` every app (for `init()` and `background()`: hidden apps keep
running), `app_registry_find(id)` an app's menu position. `app_registry_menu_json()` and
`app_registry_save_prefs()` are for the portal and safe from its task (a mutex guards the
lists). `main.c` adds the native
apps; the script host adds and removes script apps (on start-up, and after an install
through the portal). Main task only.

### Lifecycle

```
power-on   init()  ->  background() (all apps)  ->  open() on the last-used app
on screen  button(), tick() when next_wakeup() expires, background() every ~30 s
leaving    HOME, or button() returning false  ->  close() (if set), then the menu.
           Keep state in statics and it's still there at the next open()
```

### Rules for each callback

- **`open`**: the menu was on screen, so draw a complete screen with a **full** refresh
  (`epd_init()` ... `epd_display()`). Reset any per-visit state here (e.g. the weather app
  clears the highlighted day).
- **`button`**: receives `BUTTON_LEFT/RIGHT/SELECT` (taps), `BUTTON_LEFT_HOLD/RIGHT_HOLD`
  and `BUTTON_BACK`. Never `BUTTON_HOME`, which main handles. Return **false only for
  BACK on your top screen**; for BACK on a deeper screen, go up a level yourself and
  return true. Use partial refreshes for small changes.
- **`next_wakeup` / `tick`**: for screens that change on their own (clock, forecast). Store
  a deadline tick and return `app_ticks_until(deadline)`; return `portMAX_DELAY` when
  nothing is scheduled (or leave both NULL). `tick()` can be called early (e.g. while a job
  is running the loop wakes every 100 ms), so always check `app_due(deadline)` first.
- **`close`**: stop whatever should only run while the app is open (the Transfer app
  stops the web server). Don't draw: the menu is about to replace the screen.
- **`BUTTON_WAKE`** arrives in `button()` when another task (e.g. the web server) posted
  it with `buttons_post()` to get the main loop's attention. Apps that don't use it just
  return true.
- **`background`**: called for **every** app about every 30 s, whether it's showing or not.
  Use it to start background work (nightly downloads, retries). Keep it fast and don't
  draw unless `app_is_showing(&your_app)`.

### Helpers

| Function | Use |
|---|---|
| `app_is_showing(&my_app)` | Should a background result redraw the screen? |
| `app_showing()` | The app on screen (NULL = the menu) |
| `app_due(deadline)` | Has this tick count passed? Safe across counter wrap-around |
| `app_ticks_until(deadline)` | Ticks left, 0 if passed; return this from `next_wakeup` |

## Worker: background jobs

The main task must never block on the network, or the buttons stop responding. Instead:

```c
static story_t download;                    // owned by the job while it runs

// 1. Runs in the worker task. Only the slow part; touch nothing the main task uses.
static void *download_job(void *arg)
{
    return fetch_story(&download) == ESP_OK ? &download : NULL;
}

// 2. Runs later in the main task (worker_dispatch()). Apply the result and redraw.
static void download_done(void *result, void *arg)
{
    if (result && app_is_showing(&story_app)) { /* copy, redraw */ }
}

// 3. Start it from any callback. Returns false if this job is already pending.
worker_submit(download_job, download_done, NULL);
```

`worker_submit_always()` skips the "already pending" check, for one job function that
serves many requests (the Lua apps' `net` module). At most 4 jobs are queued at once,
shared by everyone; `worker_free_slots()` says how many more fit.

- Jobs run **one at a time** in submission order, on a 12 KB stack (enough for HTTPS).
  A long job (writing a story, ~1 min) delays the jobs queued behind it.
- A job function can only be pending once: `worker_submit()` returns false for a duplicate,
  and `worker_pending(job)` tells you whether one is in flight (e.g. to show "Writing...").
- `done()` runs even if the user has left the app; check `app_is_showing()` before drawing.
- `arg` is passed to both functions. To pass a small number, cast it:
  `(void *)(intptr_t)value`. For anything bigger, `malloc` a struct in the job, return it,
  and `free` it in `done()` (see `app_story/story_app.c`).
- Up to 4 jobs can be pending at once (`MAX_JOBS` in `worker.c`).

## Settings (NVS)

```c
int32_t level = settings_get_i32("hsk_level", 3);    // 3 if never set
settings_set_i32("hsk_level", 4);                    // skips the write if unchanged
```

Stored in the NVS partition, namespace `"app"`. Keys are at most 15 characters and shared
by all apps, so prefix them if they might clash. Keys in use: `last_app_id` (string), `on_menu` (main), `apps_seeded` (app_script),
`menu_order`, `menu_hidden` (strings, app registry),
`utc_off` (local_time), `rotate180` (Settings app, applied by main), `hsk_level` (story), `bk<book ID>`, `bo<book ID>`, `book_seq` (reading positions, last opened; books). Good for small values that change rarely;
use files for anything larger.

## Storage (`/store`)

`storage_init()` (called by main) mounts the 1 MB `storage` partition as SPIFFS at
`STORAGE_PATH` (`"/store"`), formatting it the first time. Then use plain C file
functions: `fopen(STORAGE_PATH "/notes.txt", "a")`.

SPIFFS has no directories; a `/` in a name is just part of the name, so prefix files by
app. At most 4 files can be open at once (`max_files`). Erasing the flash
(`idf.py erase-flash`) wipes these files; reflashing the firmware does not.

Files in use: `story.json`, `known.txt` (story), `review.txt` (review), `flashcards.txt` (flashcards).

## Local time

```c
struct tm t;
if (local_time_now(&t)) { /* t.tm_hour, t.tm_min ... in local time */ }
```

- The clock comes from the internet (SNTP, started in `main.c`, hourly), from the
  board's clock chip after a reset (`components/clock_chip`), or by hand
  (`local_time_set_local()`, the Settings app). Internet syncs and manual changes are
  copied to the clock chip. `local_time_synced()` is false until the clock is set.
- `local_time_format_hm()` gives "5:05 PM" for status bars (`LOCAL_TIME_24H` for "17:05").
- The **time zone** comes from the weather forecast: the Weather app (Lua, `time.set_zone()`) calls
  `local_time_set_offset()` with each one. It's remembered in flash, so after a power-on
  the clock and story use the last known time zone straight away.
  `local_time_offset_known()` is false only before the very first forecast.
