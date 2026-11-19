# app_script: Lua apps

Runs apps written in Lua 5.4 (`components/lua`) that users install through the web
portal, without rebuilding the firmware. **App authors: see [docs/api.md](../../docs/api.md).**

| File | What it does |
|---|---|
| `include/app_script.h` | For main.c (start, rescan) and the web portal (stage, install, remove, list, log) |
| `script_host.c` | Finds installed apps, gives each an `app_t`, runs its Lua safely, the error screen, installing, bundled apps, logs |
| `script_api.c` | The API apps program against: `screen`, `ui`, `app`, `time`, `battery`, `storage`, `settings`, `print`/`log` |
| `script_internal.h` | `script_t` (one per installed app), shared by the two |

## Where apps live

A 2 MB FAT partition (`apps` in `partitions.csv`) mounted at `/apps`:

```
/apps/<id>/app.json        manifest
/apps/<id>/main.lua        program (+ other .lua modules)
/apps/<id>/icon.bin        32x32 1-bit icon (the portal converts icon.png)
/apps/<id>/settings.json   setting values (portal, later)
/apps/<id>/data/           the app's files (storage module)
/apps/.staging/<id>/       a package being uploaded
```

## How an app runs

At start-up `app_script_init()` scans `/apps`. For each folder with a readable `app.json`
it creates a `script_t`, whose `app_t` goes into the app registry (`app_registry.h`) after
the native apps, sorted by name. `main.c` then treats it like any other app. The `app_t`
callbacks are the same functions for every script app; `self->ctx` says which one.

- **Loading** (`load()`): a fresh Lua state whose allocator draws from PSRAM, capped at
  512 KB per app. Only safe libraries are opened (no io/os/debug, no dofile/loadfile), and
  `require` looks in the app's folder. The API is registered, then `main.lua` runs.
- **Calls** (`call()`): every callback goes through `lua_pcall` with a traceback handler.
  An instruction-count hook (every 1000 instructions) aborts a call after 10 M
  instructions.
- **Failure** (`fail()`): the error goes to the log; the state is closed (all its memory
  freed); `failed` is set, so the tile shows a "!" badge. If the app is on screen, the error
  screen shows the message, and the next press leaves. Opening the app again starts it
  fresh.
- **Lifetime**: loaded when opened and unloaded when closed, unless the manifest says
  `"background": true`. Those are loaded at start-up and get `on_background()` with every
  native app's `background()`.
- **Manifest needs a newer API**: the app is listed but `broken`; opening it explains.

Timers: `app.wake_in(ms)` sets `wake_at`; the host's `next_wakeup()`/`tick()` turn it into
`on_tick()`. `app.close()` sets `close_requested` and posts `BUTTON_WAKE`, so the main loop
calls `button()`, which returns false.

## Installing (web portal task)

The portal only runs while the Transfer app is open, so no script app is on screen while
its files change.

1. `app_script_stage()`: chunked upload of each file into `/apps/.staging/<id>/` (same
   resume rule as books: chunks must arrive in order; out of step returns the size held).
2. `app_script_install()`: checks the staged `app.json` (id matches, has a name, `api` ≤
   `APP_SCRIPT_API_VERSION`) and that `main.lua` exists. Then it deletes the old program
   files (keeping `settings.json` and `data/`) and moves the staged files in.
3. The portal posts `BUTTON_APPS_CHANGED`; the main loop calls `app_script_rescan()`, which
   removes every script app from the registry, frees them, and scans again.

`app_script_uninstall()` deletes the folder with its settings and data.

## Bundled apps

Apps in the repository's `apps/` folder can be built into the firmware (`CMakeLists.txt`
embeds `app.json`, `main.lua`, `icon.bin`; `bundled[]` in `script_host.c` lists them).
On start-up each is installed once (bit i of the `apps_seeded` setting remembers it, so an
uninstalled one stays gone), and updated when the firmware's copy has a higher `version`
than the installed one. Today: **Clock**.

To bundle another: add its folder name to the `foreach` in `CMakeLists.txt` and an entry
to `bundled[]`, at the end (the bit position is its index).

## Logs

`print()`, `log()` and errors go to the serial log (`app_script: [<id>] ...`) and to a 2 KB
ring buffer per app (8 apps at a time), which the portal shows (`GET /api/apps/log`). The
buffers are kept apart from the apps, behind a mutex, so the portal's task can read them
while the main task reloads apps.
