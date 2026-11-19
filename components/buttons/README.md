# buttons: thumbwheel, EXIT and MENU

A background task reads the CrowPanel's five inputs every 10 ms, debounces them, and turns
presses into events on a queue. The main loop collects them with `buttons_wait()`.

## Events

The event names say what they mean to apps ("previous", "next", "back"...), not which
control produced them, so apps don't depend on the hardware:

| Control | Tap | Hold ½ s |
|---|---|---|
| Thumbwheel **up** | `BUTTON_LEFT` (previous) | `BUTTON_LEFT_HOLD` |
| Thumbwheel **down** | `BUTTON_RIGHT` (next) | `BUTTON_RIGHT_HOLD` |
| Thumbwheel **press** | `BUTTON_SELECT` | |
| **EXIT** | `BUTTON_BACK` | |
| **MENU** | `BUTTON_HOME` | |

- Controls **with** a hold gesture send their tap on **release**, so a press that turns
  into a hold doesn't also tap. The hold fires while still held, once per press.
- Controls **without** one (wheel press, EXIT, MENU) send their event as soon as they're pressed.

## API

```c
buttons_init();                                   // once at start-up
button_event_t e;
if (buttons_wait(&e, pdMS_TO_TICKS(1000))) { }    // true if an event arrived in time
```

The queue holds 8 events; presses beyond that while the main loop is busy are dropped.

## Pins and timing (`buttons.c`)

| Control | GPIO | Net (Elecrow schematic) |
|---|---|---|
| Wheel up | 6 | `IO6_UP` |
| Wheel down | 4 | `IO4_DOWN` |
| Wheel press | 5 | `IO5_CONF` |
| EXIT | 1 | `IO1_EXIT` |
| MENU | 2 | `IO2_MENU` |

Each pulls its pin to GND when pressed; the board has pull-up resistors and the internal
pull-ups are enabled too.

| Constant | Value | Meaning |
|---|---|---|
| `POLL_MS` | 10 | How often the pins are read |
| `DEBOUNCE_MS` | 30 | A new level must be steady this long to count (filters contact bounce) |
| `LONG_PRESS_MS` | 500 | How long a hold takes |

## Changing the mapping

Everything is in the `buttons[]` table in `buttons.c`: one row per input with its pin, tap
event and hold event (`NO_HOLD` for none). For example, to make the wheel's up/down feel
reversed, swap the `tap`/`hold` events of the two wheel rows.

To add a new event (say, a hold on MENU for "settings"):

1. Add it to `button_event_t` in `include/buttons.h`.
2. Set it as the `hold` of that row (MENU's tap then moves from on-press to on-release).
3. Handle it in the apps that need it; the others ignore events they don't know.

Combinations (two controls at once) aren't supported by the table; the earlier
three-button version of this file had a LEFT + RIGHT combo, which is the pattern to copy
(mark both inputs `used` and send once both have been held `LONG_PRESS_MS`).
