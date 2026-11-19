# app_weather: the Weather app

A 7-day forecast from [Open-Meteo](https://open-meteo.com) (free, no API key), with a
details screen per day. It also supplies the **time zone** for every other app.

| File | What it does |
|---|---|
| `weather_app.c`, `include/weather_app.h` | The `app_t`: when to download, which screen, buttons |
| `weather_api.c/.h` | Builds the request URL, downloads, parses the JSON into `weather_t` |
| `weather_ui.c/.h` | Draws the forecast grid, the day details and graph, icons, error screens |

## Screens and buttons

```
FORECAST  |Today| Tue | Wed | ... |      wheel up/down  highlight a day (wraps around)
          | icon| icon| icon|     |      press wheel    open its details
          |  73 |  60 |  59 |     |      EXIT           leave the app
          |  43 |  44 |  39 |     |
          | o1% | o2% | ... |     |      rain chance, wind speed, wind arrow
DAY       stats on the left, 24-hour temperature / rain graph on the right
                                         wheel up/down  previous/next day
                                         EXIT           back to the forecast
```

## When it downloads

| What | When | Refresh |
|---|---|---|
| Full forecast | Opening the app, if older than `CONFIG_WEATHER_REFRESH_MINUTES` (60); then every 60 min while showing | **Full** (also clears ghosting) |
| Current conditions | Every `CONFIG_WEATHER_PARTIAL_SECONDS` (300) while showing, at most once a minute | Partial |
| First forecast | At power-on, whatever app is showing; retried every 30 s until it works | Full, if showing |
| After a failed full update | Retried after 5 min; the old forecast stays on screen | |

Each download is the same request (forecast + current conditions together, ~6.5 KB), so a
"partial" update is really a full download followed by a partial redraw.

Downloads run on the worker (`download_job()` → `download_done()`). The worker fills a
separate `download` struct; `download_done()` copies it into `weather` only on success,
so a failed download never wipes what's on screen. `full_pending` remembers whether the
download in flight should end with a full or a partial refresh.

**Opening the app** draws the cached forecast immediately (full refresh), then downloads
in the background if it's stale; the new data appears with a partial refresh.

## Time zone

The request uses `timezone=auto`, so Open-Meteo answers in the location's local time and
reports `utc_offset_seconds`. `download_done()` passes that to `local_time_set_offset()`,
which the clock and story apps use, and which is remembered across power-offs.

## Settings (menuconfig → *Weather Display*)

| Setting | Use |
|---|---|
| Latitude / longitude | Forecast location, decimal degrees |
| Units | Fahrenheit/mph/inch or Celsius/km/h/mm |
| Minutes between full refreshes | Full forecast download + full refresh |
| Seconds between partial refreshes | Current conditions + partial refresh |

(The Wi-Fi settings in the same menu are used by `components/net`.)

## Extending it

- **More data from Open-Meteo:** add the field name to `WEATHER_URL` in `weather_api.c`
  (e.g. `&daily=...,snowfall_sum`), a member to `weather_day_t` / `weather_t` in
  `weather_api.h`, and read it in `parse_response()` with `array_int()` / `array_float()`.
  The response buffer is 16 KB (`RESPONSE_MAX`); the current reply is ~6.5 KB.
- **New icons:** `weather_ui.c` maps WMO weather codes to a few icon types and draws them
  with lines and circles; add a case to the code → icon mapping and a drawing routine.
- **Another screen** (e.g. hourly for today): add a flag like `in_day`, handle its
  buttons in `weather_button()` (BACK returns to the forecast), and make
  `weather_next_wakeup()` return `portMAX_DELAY` while it's showing if it doesn't update
  on its own.
