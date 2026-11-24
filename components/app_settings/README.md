# app_settings: Settings app

Set the date and time by hand, turn the screen upside down, choose the music, podcasts and
e-books folders, and format the microSD card.

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

- **Podcasts** picks the podcasts folder the same way. The feed list and the feeds' folders
  move along (other files in the old folder stay). A podcast that's playing is stopped first.
- **Books** picks where e-books live: a folder on the card (default `/Books`) or **Internal
  memory** (the flash partition). The books move along, with their page indexes and pictures.
- **Voice** picks the voice notes folder (default `/Voice Notes`); the notes move along.
- **Refresh** picks how quick (partial) refreshes are done, to compare on the panel: **Stock**
  (the chip's own waveform, ~890 ms, nudges every pixel), **Clean** (Waveshare's waveform for this
  panel, ~830 ms, leaves unchanged pixels nearly alone), **Quick** (~630 ms) and **Quickest**
  (~430 ms: the same with a shorter drive, possibly a little faint). The log shows each refresh's time.
- **Darkness** is a slider (1 lightest .. 5 untouched): press to adjust, left / right to move it,
  press or BACK when done. It applies to every picture (album art, book pictures, the off screen photo).
- **Card** shows the mounted card's size ("not in" without one). Press: unmount and mount it
  again, e.g. after swapping cards (apps also notice a swapped card by themselves: see
  `sdcard_mount()`). The result shows under the list.
- **Format** erases the card: a confirmation screen, then **hold the center button** (a hold
  that started less than a second after the screen opened is ignored, so one long press
  can't both open it and confirm it). The player is stopped, the card gets a fresh FAT32, and
  every app's folder is made again, empty: music, podcasts, e-books (when on the card), voice
  notes and `/Playlists`, including nested ones like `/Music/Rock`.

The list shows four rows at a time and scrolls (a bar on the right shows where you are). Under
the rows: when the clock was last set from the internet and the time zone; on a folder row,
its full path; after an action, its result.

## How a manual time interacts with internet time

`local_time_set_local()` sets the system clock and the clock chip, using the current
time zone (UTC if none is known yet). While online, SNTP re-checks every hour and its time
replaces the manual one, which is what you want once the internet is back. Offline, the
manual time holds, through resets and power-offs too, thanks to the clock chip.

The time zone isn't set here: it comes with every weather forecast (see
`components/app_core/include/local_time.h`).
