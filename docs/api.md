# Writing apps (API 1)

Apps are small Lua 5.4 programs. You write them in any text editor, put them in a folder
or a `.zip`, and install them from the board's web page (**Transfer** app → **Apps** →
**Install an app**). No compiler or SDK is needed. The built-in Clock
([apps/clock](../apps/clock)) is a complete example.

## The package

```
myapp/
  app.json     manifest (required)
  main.lua     the program (required); more .lua files load with require("name")
  icon.png     menu icon (optional): any size, converted to 32x32 black and white on install
  README.md    for people (optional, not sent to the board)
```

File names may use letters, digits, `.`, `_` and `-` (up to 32 characters). Files must sit
at the top of the package: there are no subfolders. Each file can be up to 256 KB. All apps
together have about 2 MB.

### app.json

```json
{
  "id": "myapp",
  "name": "My App",
  "version": "1.0.0",
  "api": 1,
  "author": "you",
  "description": "One line about what it does.",
  "background": false,
  "settings": [
    {"key": "h24", "label": "24-hour clock", "type": "bool", "default": false}
  ]
}
```

| Field | | |
|---|---|---|
| `id` | required | 1-24 of `a-z`, `0-9`, `_`. The app's folder on the board and its identity: installing a package with the same id updates the app |
| `name` | required | Menu label. Keep it short (about 9 characters fit under a tile) |
| `api` | required | The API version the app needs (this document: **1**). A board with an older firmware refuses to install it |
| `version` | | Shown in the portal. Dotted numbers, e.g. `1.2.0`; bundled apps update when the firmware brings a higher one |
| `author`, `description` | | Shown in the portal |
| `background` | | `true`: the app stays loaded and `on_background()` runs about every 30 s, even while it isn't open. Default `false`: loaded when opened, unloaded when closed |
| `settings` | | Settings the user can change (`settings.get(key)`). Each has `key`, `label`, `type` (`bool`, `int`, `string`, `secret`, `choice`) and `default`. Editing them in the portal comes in a later firmware; until then apps get the defaults |

## How an app runs

The board calls global functions your `main.lua` defines. All of them are optional.

| Function | Called |
|---|---|
| `on_open()` | The app was opened from the menu. Draw the first screen with a **full** refresh |
| `on_button(name)` | A button press while the app is open: `"left"`, `"right"`, `"select"`, `"back"`, `"left_hold"`, `"right_hold"`. Return `false` to leave the app, `true` to stay. Returning nothing keeps the default: `"back"` leaves, the others stay |
| `on_tick()` | The time set with `app.wake_in(ms)` has come (only while the app is open) |
| `on_background()` | About every 30 s, open or not, for apps with `"background": true` |
| `on_close()` | The app is leaving the screen (BACK, HOME, or `app.close()`) |

Without `on_button`, BACK leaves and the other buttons do nothing. Holding BACK (HOME)
always goes to the menu and never reaches the app. The lock switch silences everything.

Top-level code in `main.lua` runs once when the app loads, before `on_open()`.

### Limits and errors

- Each app has 512 KB of memory for Lua.
- One call may run about 10 million Lua instructions (a few seconds); longer is taken as
  an endless loop.
- Any error (syntax, runtime, out of memory, too long) stops the app. Its screen then
  shows the message, any button returns to the menu, and its tile gets a "!" badge until it
  runs again. The rest of the board carries on.
- `print()` / `log()` output and errors appear in the serial log and under **Log** in the
  portal.

### Lua libraries available

`string`, `table`, `math`, `utf8`, `coroutine`, `require` (your own modules), and the
basics (`pairs`, `ipairs`, `tostring`, `tonumber`, `pcall`, `error`, `select`, `type`,
`setmetatable`, `load` for strings, ...). Not available: `io`, `os`, `debug`, `dofile`,
`loadfile`. Use `storage` for files and `time` for the clock.

## The screen

296 × 128 pixels, black and white, e-paper. Drawing goes into a buffer; nothing changes on
the panel until `screen.finish()`.

```lua
screen.begin(true)                 -- start a frame: true = full refresh, false = partial
screen.text(10, 10, "Hello", 2)
screen.finish()                    -- show it
```

**Full vs partial.** A full refresh takes about 3 s and flashes black and white, but leaves
a clean picture. A partial refresh takes about 0.6 s with no flash, but leaves faint
ghosts that build up. Use full when the whole screen changes (in `on_open()`), and partial
for small changes (a cursor, a number). Do a full one now and then to clear the ghosts,
e.g. every hour or every 10 partials. `screen.begin(false)` quietly turns into a full
refresh when a partial isn't possible.

Colors: `screen.BLACK` (0, the default) and `screen.WHITE` (1).

| Function | |
|---|---|
| `screen.begin(full)` | Starts a frame: wakes the panel for a full refresh, clears the buffer to white. Returns whether it will be a full refresh |
| `screen.finish()` | Sends the buffer to the panel with the refresh `begin` chose |
| `screen.clear([color])` | Fills the buffer (white by default) |
| `screen.pixel(x, y, [color])` | |
| `screen.line(x0, y0, x1, y1, [color])` | |
| `screen.rect(x, y, w, h, [fill], [color])` | Outline, or filled with `fill = true` |
| `screen.circle(cx, cy, r, [color])` | Filled disc |
| `screen.text(x, y, text, [scale], [color])` | ASCII in the 5×7 font, `scale` 1-16 (6 px per character × scale). Returns the x after the text |
| `screen.text_width(text, [scale])` | |
| `screen.text16(x, y, text, [color])` | UTF-8 in the 16 px Unicode font: Chinese, pinyin, accented Latin. Chinese characters are 16 px wide, Latin 8 px. Returns the x after the text |
| `screen.text16_width(text)` | |
| `screen.bitmap(x, y, w, h, data, [color])` | 1-bit image from a string: `ceil(w/8)` bytes per row, top to bottom, most significant bit leftmost, 1 = draw |
| `screen.width`, `screen.height` | 296, 128 |

### ui: the board's look

| | |
|---|---|
| `ui.sidebar(title, [counter])` | The title bar down the left edge (18 px), reading bottom to top, with an optional small counter (e.g. `"3/12"`) at its top end. Use it on screens that have a title |
| `ui.content_x`, `ui.content_w` | Where content starts right of the bar, and its width |
| `ui.loading()` | A small "Loading..." box over the current screen (partial refresh). Show it before slow work |

## Time, battery, storage, settings

| Function | |
|---|---|
| `time.now()` | Seconds since 1970 (UTC) |
| `time.date([t])` | Local date and time as `{year, month, day, hour, min, sec, wday}` (`wday` 1 = Sunday), for now or for `t`. `nil` while the clock isn't set |
| `time.synced()` | Whether the clock is set (from the internet, the clock chip or by hand) |
| `time.format_hm([t])` | `"5:05 PM"`, or `nil` while the clock isn't set |
| `time.zone()` | The time zone: seconds to add to UTC |
| `time.set_zone(seconds)` | Sets the board's time zone. For apps that know the location (a weather app) |
| `battery.percent()` | 0-100, or `nil` without a battery |
| `battery.state()` | `"none"`, `"discharging"`, `"charging"` or `"full"` |
| `battery.millivolts()` | |
| `storage.write(name, text)` | Saves a file in the app's own folder (replacing it). Returns `true` on success. Kept across updates; removed with the app |
| `storage.read(name)` | The file's contents, or `nil` |
| `storage.remove(name)` | |
| `storage.list()` | Array of the app's file names |
| `settings.get(key)` | A setting's value (the manifest's `default` until the user changes it) |
| `app.wake_in(ms)` | Calls `on_tick()` in `ms` milliseconds (replaces any earlier request; a negative value cancels). One shot: ask again from `on_tick()` for a repeating timer |
| `app.close()` | Leaves the app (back to the menu) |
| `app.id`, `app.name`, `app.version`, `app.api` | |
| `print(...)`, `log(...)` | A line in the app's log |

Storing a table: there's no JSON module in API 1 yet (it comes with networking). Small
data can be written as Lua, e.g. `storage.write("state", string.format("return {n=%d}", n))`,
and read back with `load(storage.read("state"))()`.

## Example: a counter

```lua
local count = tonumber(storage.read("count") or "0")

local function draw(full)
  screen.begin(full)
  ui.sidebar("Counter")
  local text = tostring(count)
  local x = ui.content_x + (ui.content_w - screen.text_width(text, 8)) // 2
  screen.text(x, 36, text, 8)
  screen.finish()
end

function on_open() draw(true) end

function on_button(b)
  if b == "right" then count = count + 1
  elseif b == "left" then count = count - 1
  elseif b == "select" then count = 0
  else return end                        -- "back": default (leave)
  storage.write("count", tostring(count))
  draw(false)
  return true
end
```

## Compatibility promise

Within API 1, functions are only ever added, never changed or removed, so an app written
for API 1 keeps working on later firmware. A breaking change would be API 2, and the board
would say so before installing.

Coming in later versions: `net` (HTTP requests), `json`, settings forms in the portal, and
an app catalog.
