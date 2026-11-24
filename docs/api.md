# Writing apps (API 1)

Apps are small Lua 5.4 programs. You write them in any text editor, put them in a folder
or a `.zip`, and install them from the board's web page (**Transfer** app → **Apps** →
**Install an app**). No compiler or SDK is needed. The built-in Weather
([apps/weather](../apps/weather)) and Clock ([apps/clock](../apps/clock)) are complete
examples.

## Quick start: from idea to a shared app

**1. Write it.** Make a folder with two files:

```
myapp/
  app.json
  main.lua
  icon.png     (optional: the menu icon, 32x32 black and white looks best)
```

`app.json` says what the app is:

```json
{
  "id": "myapp",
  "name": "My App",
  "version": "1.0.0",
  "api": 1,
  "author": "you",
  "description": "One line about what it does."
}
```

`main.lua` says what it does. This one counts: right adds one, left takes one away, press
resets, and the count is saved:

```lua
local count = tonumber(storage.read("count") or "0")

local function draw(full)
  screen.begin(full)
  ui.sidebar("Counter")
  screen.text(ui.content_x + 20, 40, tostring(count), 8)
  screen.finish()
end

function on_open() draw(true) end

function on_button(b)
  if b == "right" then count = count + 1
  elseif b == "left" then count = count - 1
  elseif b == "select" then count = 0
  else return end                -- "back": leave the app
  storage.write("count", tostring(count))
  draw(false)
  return true
end
```

**2. Try it on your board.** Open the **Transfer** app and go to the address it shows in a
browser on the same Wi-Fi. Under **Apps → Install an app**, pick the folder (or a `.zip` of
it). The app is on the board's menu straight away. **Log** shows your `print()` output and
any errors; if the app crashes, the board shows the error and its line, and everything else
keeps working. Edit, install again, repeat: the app's saved files and settings survive.

**3. Share it.** Either:

- **Send the zip.** Anyone can install it the same way.
- **Publish it in the catalog**, so it appears on every board's **App catalog** page: fork
  the repository, add your folder as `apps/<your id>/`, run
  `python tools/catalog.py check`, and open a pull request. The same check runs on the pull
  request; once it's merged, the catalog rebuilds. Details: [Sharing your app](#sharing-your-app)
  and [apps/README.md](../apps/README.md).

**4. Update it.** Raise `version` in `app.json` and install again (or open another pull
request). Boards with the catalog's older version get an **Update** button.

The rest of this page is the reference: everything an app can use.

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
| `settings` | | Settings the user changes on the board's web page (**Apps** → **Settings**), read with `settings.get(key)`. See [Settings](#settings) |

## How an app runs

The board calls global functions your `main.lua` defines. All of them are optional.

| Function | Called |
|---|---|
| `on_open()` | The app was opened from the menu. Draw the first screen with a **full** refresh |
| `on_button(name)` | A button press while the app is open: `"left"`, `"right"`, `"select"`, `"back"`, `"left_hold"`, `"right_hold"`, `"select_hold"`. Return `false` to leave the app, `true` to stay. Returning nothing keeps the default: `"back"` leaves, the others stay. **`"select_hold"` is different:** return `true` if you handled it; otherwise the same press arrives again as `"select"`, so apps that ignore holds treat a long press as a press |
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
| `screen.image(file, x, y, [max_w], [max_h], [mode])` | A baseline JPEG from the app's data folder (saved with `storage.write`) or its package, shrunk to fit `max_w` x `max_h` (default: the screen) and dithered. `mode` `"grey"` (default: 4 greys, shown by a **full** refresh; a partial one shows it in black and white) or `"bw"` (dithered to black and white: looks right with partial refreshes). Returns the drawn width and height, or `nil` and a reason (PNGs and progressive JPEGs aren't supported) |
| `screen.image_data(jpeg, x, y, [max_w], [max_h], [mode])` | The same from a string, e.g. the body of a `net.get` |
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
| `time.set_zone(seconds)` | Sets the board's time zone. For apps that know the location (Weather does this) |
| `time.uptime()` | Milliseconds since the board started. Never jumps (`time.now()` does when the clock gets set), so measure intervals with it |
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
| `app.showing()` | Whether the app is on screen right now (a background download uses it to decide whether to redraw) |
| `app.id`, `app.name`, `app.version`, `app.api` | |
| `print(...)`, `log(...)` | A line in the app's log |

Storing a table: `storage.write("state", json.encode(t))`, and later
`t = json.decode(storage.read("state") or "{}")`.

## Settings

Declare them in `app.json`; the board's web page builds a form from them (**Transfer** →
**Apps** → **Settings**) and saving restarts the app with the new values.

```json
"settings": [
  {"key": "city", "label": "City", "type": "string", "default": "Oslo"},
  {"key": "metric", "label": "Metric units", "type": "bool", "default": true},
  {"key": "interval", "label": "Minutes between updates", "type": "int", "min": 5, "max": 120, "default": 30},
  {"key": "style", "label": "Style", "type": "choice", "default": "big",
   "options": [{"value": "big", "label": "Big digits"}, {"value": "small", "label": "Small"}]},
  {"key": "token", "label": "API token", "type": "secret"}
]
```

| Type | Form field | `settings.get()` returns |
|---|---|---|
| `bool` | checkbox | `true` / `false` |
| `int` | number, checked against `min` / `max` | an integer |
| `string` | text (up to 512 characters) | a string |
| `choice` | list of `options` (plain strings, or `{"value", "label"}`) | the chosen value |
| `secret` | password field. The board never sends it back to the page; leaving it empty keeps the saved one. Never give it a `default` | a string, or `nil` |

Until the user saves a value, `settings.get()` returns the `default` (or `nil` without one).
Settings are kept when the app is updated, and removed with it.

## Network and JSON

Requests run in the background, so the screen and buttons keep working; the answer arrives
later in a callback.

```lua
local ok, why = net.get("https://api.example.com/today?city=" .. settings.get("city"), nil,
  function(status, body)
    if status ~= 200 then
      log("failed: " .. tostring(status or body))   -- status is nil when there was no answer
      return
    end
    local data = json.decode(body)
    -- ... use data; redraw with screen.begin(false) ... screen.finish() if app.showing()
  end)
if not ok then log("couldn't start: " .. why) end     -- e.g. too many requests at once
```

| Function | |
|---|---|
| `net.get(url, headers, callback)` | Starts an HTTP or HTTPS GET. `headers` is `nil` or a table like `{["Accept"] = "application/json"}`. Returns `true`, or `false` and a reason if it can't start |
| `net.post(url, headers, body, callback)` | The same, sending `body` (a string, up to 64 KB). Set `Content-Type` in `headers` |
| `callback(status, body)` | Called later: `status` is the HTTP status (200, 404, ...) and `body` the answer; or `status` is `nil` and `body` says why there was no answer |
| `json.decode(text)` | JSON to Lua: objects become tables with string keys, arrays become tables indexed from 1, `null` becomes `nil`. Raises an error saying where the text is wrong (wrap it in `pcall` for untrusted input) |
| `json.encode(value)` | Lua to JSON. A table with exactly the keys 1..n becomes an array; any other table an object (keys must be strings or numbers); an empty table becomes `{}` |

- HTTPS servers are checked against the board's built-in root certificates.
- Answers can be up to 256 KB; a request gives up after 15 s without data.
- One app can have 2 requests in flight. Redirects aren't followed: use the final URL.
- If the app is closed (and unloaded) before the answer arrives, the callback is dropped.
  Apps with `"background": true` stay loaded, so their callbacks always arrive.
- The answer's text and anything decoded from it count toward the app's 512 KB.

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

## Sharing your app

Published apps live in the `apps/` folder of this repository, one folder per app, the same
layout as any package. Every change is checked automatically, and the board's web page
installs from the result (**Transfer** → **App catalog**).

1. Write and test your app (install it from a folder on your board).
2. Fork the repository, add `apps/<your id>/` with `app.json`, `main.lua`, `icon.png` and
   optionally a `README.md`, and open a pull request.
3. A check runs on the pull request (`tools/catalog.py check`): manifest rules, file names
   and sizes, a Lua syntax check, an icon, and for an update, a higher `version` than
   before. Run it yourself first: `python tools/catalog.py check`.
4. Once merged, the catalog rebuilds and the app appears on every board's **App catalog**
   (boards with an older version installed see **Update**).

The catalog is a JSON file at `https://maclogan.github.io/utilreader/index.json` listing each
app with a download link and a checksum. Anyone can host one (`python tools/catalog.py build
<folder>` makes it from an `apps/` folder) and point the board's web page at its address.

## Compatibility promise

Within API 1, functions are only ever added, never changed or removed, so an app written
for API 1 keeps working on later firmware. A breaking change would be API 2, and the board
would say so before installing.

API 1 also covers `net`, `json`, `time.uptime()` and `app.showing()`.
