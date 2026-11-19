# buttons: 5-way switch, BACK / HOME and lock

A background task reads the controls every 10 ms, debounces them (a reading must hold for
30 ms), and turns them into events on a queue that `app_main` reads with `buttons_wait()`.
The controls are wired to the board's 14-pin header; see "Wiring the controls" in the
top-level README.

## Events

| Control | Tap | Hold (500 ms) |
|---|---|---|
| 5-way **left** | `BUTTON_LEFT` (previous) | `BUTTON_LEFT_HOLD` |
| 5-way **right** | `BUTTON_RIGHT` (next) | `BUTTON_RIGHT_HOLD` |
| 5-way **center** | `BUTTON_SELECT` | |
| **BACK / HOME** button | `BUTTON_BACK` | `BUTTON_HOME` |
| Board **KEY** button | `BUTTON_BACK` | `BUTTON_HOME` |

- Controls **with** a hold gesture send their tap on release, so a press that turns into a
  hold doesn't also tap. The hold fires while the control is still held.
- Controls **without** one (center) send their event as soon as they're pressed.
- `BUTTON_WAKE` isn't a control: other tasks post it with `buttons_post()` to make the main
  loop look for work (the web interface uses it).
- `BUTTON_LOCK_CHANGED` is sent when the lock switch moves. `main.c` uses it to redraw the
  menu's padlock; apps never see it.

## Lock switch

While the lock switch is closed (pin at GND), every press is dropped; `buttons_locked()`
says whether it's on. A control that is held when the switch moves stays silent until it's
released, so locking or unlocking mid-press never produces a stray event.

## Power

- `buttons_init()` ignores a control that's already held at start-up until it's released,
  so the BACK / HOME press that turned the device on doesn't also send `BUTTON_HOME`.
- `buttons_power_on_held()` and `buttons_prepare_wake()` are the buttons' part in turning
  on and off; `components/power` calls them. Before deep sleep, the 5-way and lock pins are
  floated (a pull-up through a closed contact would drain the battery), and GPIO2 (header
  button) and GPIO1 (KEY) are armed to wake the chip when pulled low. GPIO2 keeps the chip's
  internal pull-up through deep sleep; KEY has a 10k pull-up on the board.

## Pins

| Control | GPIO | Header pin |
|---|---|---|
| 5-way left | 4 | GP4 |
| 5-way right | 5 | GP5 |
| 5-way center | 3 | GP3 |
| BACK / HOME | 2 | GP2 |
| Lock switch | 44 | RX |
| KEY (on the board) | 1 | |

All connect the pin to GND when pressed (or locked); the firmware enables the pull-ups.
Only GPIO0-21 can wake the chip from deep sleep, which is why the power button is on GPIO2
and the lock switch, which never needs to wake it, takes the RX pin.

## Changing the controls

Edit the `PIN_*` defines and the `buttons[]` table in `buttons.c`. A new control is one
line in the table: its pin, its tap event, and its hold event (or `NO_HOLD`). If you move
the power button, keep it on GPIO0-21 and update `POWER_PINS`.
