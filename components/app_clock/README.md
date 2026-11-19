# app_clock: the Clock app

The local time in big digits with the date underneath.

| File | What it does |
|---|---|
| `clock_app.c`, `include/clock_app.h` | The `app_t`: when to redraw |
| `clock_ui.c/.h` | Drawing: the time at 7× the 5×7 font, the date at 2×, the menu icon |

```
+-----------------------------------------+
|      ##  ##### ##  #####   PM           |   "12:05", AM/PM beside it
|            Tuesday Oct 27               |
+-----------------------------------------+
```

## How it updates

- `clock_next_wakeup()` returns the ticks until the next minute starts (+200 ms so the
  redraw lands just after the change), so the main loop sleeps until then.
- `clock_tick()` redraws with a **partial** refresh each minute, and a **full** refresh at
  the top of each hour to clear ghosting.
- If the time zone changes (`local_time_offset()` differs from the one last drawn), it
  redraws immediately. That happens when the first forecast after a power-on corrects
  the remembered time zone.
- Before internet time has synced it shows dashes.

No controls do anything except EXIT / MENU (back to the menu).

## Changing it

- **24-hour time:** set `CLOCK_24H` to 1 in `clock_ui.c`.
- **Size and position:** `TIME_SCALE`, `TIME_Y`, `DATE_Y` in `clock_ui.c`.
- **Seconds:** don't. A partial refresh every second would ghost badly and keep the panel busy.
- **Adding information** (e.g. today's high/low): the clock can't include the weather app's
  headers (apps don't depend on each other). Put shared data in `app_core` instead, the way
  the time zone is shared through `local_time.h`.
