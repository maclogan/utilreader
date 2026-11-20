# app_settings: Settings app

Set the date and time by hand, turn the screen upside down, and choose the music folder.

| File | What it does |
|---|---|
| `settings_app.c`, `include/settings_app.h` | The `app_t`, its screen and its menu icon (a gear) |

## Using it

| Screen | Left / right | Hold left / right | Press | BACK |
|---|---|---|---|---|
| **Rows** | Choose Date, Time, Rotate or Music | Same | Date/Time: start editing. Rotate: flip the screen. Music: pick a folder | Main menu |
| **Editing** | Change the boxed field by 1 | Change it by more (year/day/minute 10, month 3, hour 6) | Next field; on the last field, save | Cancel |

- **Date** edits year, month, day (2024-2099; the day is kept valid for the month).
  Saving keeps the current time of day.
- **Time** edits hour and minute; AM/PM follows the hour. Saving sets the seconds to 0
  and keeps the date.
- **Rotate** flips the picture 180° straight away (for an enclosure that holds the board
  the other way up) and remembers it in flash (`SETTING_ROTATE_180`). `main.c` applies it
  at start-up before anything is drawn, so the "off" screen matches too.

- **Music** opens a folder picker on the microSD card: the first entry, **Use this
  folder**, chooses the folder you're in; the others open a subfolder; BACK goes up a level
  (and out, from the top). Saved as `SETTING_MUSIC_DIR` (default `/Music`); the Music app
  rescans when it next opens.

Under the rows: when the clock was last set from the internet, and the time zone.

## How a manual time interacts with internet time

`local_time_set_local()` sets the system clock and the clock chip, using the current
time zone (UTC if none is known yet). While online, SNTP re-checks every hour and its time
replaces the manual one, which is what you want once the internet is back. Offline, the
manual time holds, through resets and power-offs too, thanks to the clock chip.

The time zone isn't set here: it comes with every weather forecast (see
`components/app_core/include/local_time.h`).
