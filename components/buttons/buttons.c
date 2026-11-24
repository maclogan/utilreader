#include "buttons.h"

#include "driver/gpio.h"                // gpio_config(), gpio_get_level()
#include "driver/rtc_io.h"              // pull-ups that stay on in deep sleep
#include "driver/usb_serial_jtag.h"      // testing: keyboard over USB serial (CONFIG_BUTTONS_SERIAL_KEYS)
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"                    // ESP_ERROR_CHECK()
#include "esp_log.h"
#include "esp_sleep.h"                  // wake-up on the BACK / HOME button
#include <stdint.h>                     // INT64_MAX
#include "esp_timer.h"                  // esp_timer_get_time(): time since boot
#include "expander.h"                   // the 5-way switch, on the I2C I/O expander
#include "freertos/queue.h"             // queue that carries events to app_main
#include "freertos/task.h"              // xTaskCreate(), vTaskDelay()

static const char *TAG = "buttons";

// Waveshare ESP32-S3-LR1121-ePaper-2.9, 14-pin header P2. Every control connects its pin to
// GND when pressed (or, for the lock switch, while locked); the pull-ups are internal.
// The 5-way switch is on the I/O expander (expander.h), which frees GPIO3-5 for audio.
// GPIO44 is the header's RX pin (UART0 receive, unused: the console is on USB).
#define EXP(bit)        (100 + (bit))   // "pin" numbers >= 100 are expander pins
#define PIN_LEFT        EXP(EXPANDER_LEFT)
#define PIN_RIGHT       EXP(EXPANDER_RIGHT)
#define PIN_CENTER      EXP(EXPANDER_CENTER)
#define PIN_BACK        2               // header GP2: BACK / HOME button, also turns the device on
#define PIN_KEY         1               // the board's KEY button (10k pull-up on the board): same as BACK / HOME
#define PIN_LOCK        44              // header RX: lock slide switch, closed to GND = locked

#define PRESSED_LEVEL   0               // pressed = pulled to GND

#define POLL_MS         10              // how often the inputs are checked
#define DEBOUNCE_MS     30              // a reading must hold steady this long to count (filters contact bounce)
#define LONG_PRESS_MS   500             // holding this long = a "hold" gesture
#define RELEASE_WAIT_MS 10000           // before sleeping, wait at most this long for the button to be let go

#define NO_HOLD         ((button_event_t)-1)

#define PIN_MASK(p)     (1ULL << (p))
#define POWER_PINS      (PIN_MASK(PIN_BACK) | PIN_MASK(PIN_KEY))

static QueueHandle_t events;            // button events waiting to be handled by app_main
static uint8_t expander_pins = 0xFF;    // last expander reading (1 = released)
static volatile bool locked = false;    // lock switch on: presses are dropped

// One physical input and the events it produces
typedef struct {
    int pin;                            // GPIO number, or EXP(bit) for an expander pin
    button_event_t tap;                 // sent on a short press
    button_event_t hold;                // sent once after LONG_PRESS_MS, or NO_HOLD
    bool pressed;                       // current debounced state
    int change_ms;                      // how long the raw reading has disagreed with `pressed`
    int held_ms;                        // how long it has been held down
    bool used;                          // this press already sent its event (or must stay silent)
} button_t;

// Inputs without a hold gesture send their tap as soon as they're pressed (snappier).
// Inputs with one send the tap on release, so a press that becomes a hold doesn't also tap.
static button_t buttons[] = {
    {.pin = PIN_LEFT,   .tap = BUTTON_LEFT,   .hold = BUTTON_LEFT_HOLD},
    {.pin = PIN_RIGHT,  .tap = BUTTON_RIGHT,  .hold = BUTTON_RIGHT_HOLD},
    {.pin = PIN_CENTER, .tap = BUTTON_SELECT, .hold = BUTTON_SELECT_HOLD},
    {.pin = PIN_BACK,   .tap = BUTTON_BACK,   .hold = BUTTON_HOME},
    {.pin = PIN_KEY,    .tap = BUTTON_BACK,   .hold = BUTTON_HOME},
};
#define BUTTON_COUNT ((int)(sizeof(buttons) / sizeof(buttons[0])))

// The lock switch, debounced the same way: how long its reading has disagreed with `locked`
static int lock_change_ms;

static bool is_low(int pin)
{
    if (pin >= EXP(0)) {
        return !(expander_pins & (1 << (pin - EXP(0))));
    }
    return gpio_get_level(pin) == PRESSED_LEVEL;
}

// Reads the expander once per poll. A failed read counts as "nothing pressed".
static void read_expander(void)
{
    uint8_t v;
    expander_pins = expander_read(&v) ? v : 0xFF;
}

static void send(button_event_t e)
{
    xQueueSend(events, &e, 0);          // 0 = don't wait if the queue is full (drop the press)
}

// Sends a press, unless the controls are locked
static void press(button_event_t e)
{
    if (!locked) {
        send(e);
    }
}

// Debounces one input and sends its events
static void update_button(button_t *b)
{
    bool raw = is_low(b->pin);                              // true while physically pressed

    if (raw == b->pressed) {
        b->change_ms = 0;                                   // reading agrees: no change pending
    } else if ((b->change_ms += POLL_MS) >= DEBOUNCE_MS) {  // new reading has lasted long enough
        b->pressed = raw;                                   // accept the new state
        b->change_ms = 0;
        if (raw) {                                          // just pressed: start a fresh press
            b->held_ms = 0;
            b->used = false;
            if (b->hold == NO_HOLD) {
                press(b->tap);                              // nothing to wait for
                b->used = true;
            }
        } else if (!b->used) {                              // just released, and it wasn't a hold
            press(b->tap);
        }
    }

    if (b->pressed) {
        b->held_ms += POLL_MS;
        if (!b->used && b->hold != NO_HOLD && b->held_ms >= LONG_PRESS_MS) {
            press(b->hold);                                 // fires while still held
            b->used = true;                                 // once per press, and no tap on release
        }
    }
}

// Debounces the lock switch. When it moves, a button held at that moment stays silent
// until released, so locking/unlocking mid-press never produces a stray event.
static void update_lock(void)
{
    bool raw = is_low(PIN_LOCK);
    if (raw == locked) {
        lock_change_ms = 0;
        return;
    }
    if ((lock_change_ms += POLL_MS) < DEBOUNCE_MS) {
        return;
    }
    lock_change_ms = 0;
    locked = raw;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (buttons[i].pressed) {
            buttons[i].used = true;
        }
    }
    ESP_LOGI(TAG, "controls %s", locked ? "locked" : "unlocked");
    send(BUTTON_LOCK_CHANGED);
}

// Runs forever: reads the inputs every POLL_MS
static void button_task(void *arg)
{
    while (1) {
        update_lock();
        read_expander();
        for (int i = 0; i < BUTTON_COUNT; i++) {
            update_button(&buttons[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

#if CONFIG_BUTTONS_SERIAL_KEYS
// ---------- testing: keys typed over USB serial act as buttons ----------
// Arrow keys arrive as ANSI escape sequences (ESC [ C / ESC [ D, or ESC O C / D).
static button_event_t key_event(int c)
{
    switch (c) {
    case 'a': case 'j':             return BUTTON_LEFT;
    case 'A': case 'J':             return BUTTON_LEFT_HOLD;
    case ',': case '<': case '[':   return BUTTON_LEFT_HOLD;      // previous track: a clean hold
    case '.': case '>': case ']':   return BUTTON_RIGHT_HOLD;     // next track
    case 'd': case 'l':             return BUTTON_RIGHT;
    case 'D': case 'L':             return BUTTON_RIGHT_HOLD;
    case '\r': case ' ': case 's':  return BUTTON_SELECT;
    case 'S':                       return BUTTON_SELECT_HOLD;
    case '\b': case 0x7F: case 'b': return BUTTON_BACK;
    case 'h': case 'H':             return BUTTON_HOME;
    default:                        return NO_HOLD;
    }
}

// Holding a key down: a terminal only sees the key repeating (after the keyboard's repeat
// delay, usually 250-1000 ms, then ~30 times a second). For the select keys (Enter, space,
// 's') the tap waits KEY_TAP_WAIT_MS: if the key repeats by then, it's a hold, like holding
// the 5-way's center. A keyboard set to a longer repeat delay gives a tap, then the hold.
#define KEY_TAP_WAIT_MS     650
#define KEY_REPEAT_GAP_MS   200     // repeats closer than this belong to the same hold
// Left / right act at once (lists must stay quick). A second press soon after the first waits
// ARROW_SETTLE_MS: if more follow quickly, the key is being held (auto-repeat): one hold
// event (skip track, jump a page), and its further repeats are absorbed.
#define ARROW_MAYBE_MS      750     // a second press this soon may be the start of auto-repeat
#define ARROW_SETTLE_MS     120     // auto-repeat comes faster than this

static bool is_select_key(uint8_t c)
{
    return c == '\r' || c == ' ' || c == 's';
}

static void serial_key_task(void *arg)
{
    int esc = 0;                    // 0 = normal, 1 = after ESC, 2 = after ESC [ or ESC O
    bool pending = false;           // a select key is down: tap or hold not decided yet
    bool held = false;              // ... and it has become a hold (absorbing its repeats)
    int64_t deadline = 0;           // ms: decide (or end the hold) at this time
    // Left / right
    button_event_t arrow = NO_HOLD; // the last arrow pressed (BUTTON_LEFT / BUTTON_RIGHT)
    int64_t arrow_at = 0;           // when
    bool arrow_pending = false;     // a quick second press, waiting to see if it's auto-repeat
    bool arrow_held = false;        // auto-repeat seen: absorbing until it stops
    while (1) {
        uint8_t c;
        int64_t now = esp_timer_get_time() / 1000;
        int64_t next = pending ? deadline : INT64_MAX;
        if (arrow_pending && arrow_at + ARROW_SETTLE_MS < next) {
            next = arrow_at + ARROW_SETTLE_MS;
        }
        TickType_t wait = next == INT64_MAX ? portMAX_DELAY : pdMS_TO_TICKS(next > now ? next - now : 0) + 1;
        if (usb_serial_jtag_read_bytes(&c, 1, wait) != 1) {
            now = esp_timer_get_time() / 1000;
            if (pending && now >= deadline) {
                if (!held) {
                    ESP_LOGI(TAG, "key select (tap)%s", locked ? " (locked, ignored)" : "");
                    press(BUTTON_SELECT);
                }
                pending = held = false;
            }
            if (arrow_pending && now >= arrow_at + ARROW_SETTLE_MS) {
                arrow_pending = false;                      // just a second press: a tap after all
                ESP_LOGI(TAG, "key %s (tap)", arrow == BUTTON_LEFT ? "left" : "right");
                press(arrow);
            }
            continue;
        }
        now = esp_timer_get_time() / 1000;
        if (c == '\n') {
            continue;                                       // the second half of a CR LF Enter
        }
        if (esc == 0 && is_select_key(c)) {
            if (!pending) {
                pending = true;                             // wait: tap or hold?
                held = false;
                deadline = now + KEY_TAP_WAIT_MS;
            } else {
                if (!held) {
                    held = true;
                    ESP_LOGI(TAG, "key select (held)%s", locked ? " (locked, ignored)" : "");
                    press(BUTTON_SELECT_HOLD);
                }
                deadline = now + KEY_REPEAT_GAP_MS;         // still repeating: still the same hold
            }
            continue;
        }
        button_event_t e = NO_HOLD;
        if (esc == 1) {
            esc = (c == '[' || c == 'O') ? 2 : 0;
        } else if (esc == 2) {
            esc = 0;
            if (c == 'D') e = BUTTON_LEFT;
            else if (c == 'C') e = BUTTON_RIGHT;
        } else if (c == 0x1B) {
            esc = 1;
        } else {
            e = key_event(c);
        }
        if (e == BUTTON_LEFT || e == BUTTON_RIGHT) {
            if (arrow_held && e == arrow && now - arrow_at < KEY_REPEAT_GAP_MS) {
                arrow_at = now;                             // still held: absorb
                continue;
            }
            if (arrow_pending && e == arrow && now - arrow_at < ARROW_SETTLE_MS) {
                arrow_pending = false;                      // auto-repeat: it's a hold
                arrow_held = true;
                arrow_at = now;
                ESP_LOGI(TAG, "key %s (held)", e == BUTTON_LEFT ? "left" : "right");
                press(e == BUTTON_LEFT ? BUTTON_LEFT_HOLD : BUTTON_RIGHT_HOLD);
                continue;
            }
            if (arrow_pending) {                            // a different key: the waiting tap goes first
                arrow_pending = false;
                press(arrow);
            }
            arrow_held = false;
            bool maybe_repeat = e == arrow && now - arrow_at < ARROW_MAYBE_MS;
            arrow = e;
            arrow_at = now;
            if (maybe_repeat) {
                arrow_pending = true;                       // decided in ARROW_SETTLE_MS
                continue;
            }
            ESP_LOGI(TAG, "key %s", e == BUTTON_LEFT ? "left" : "right");
            press(e);
            continue;
        }
        if (e != NO_HOLD) {
            ESP_LOGI(TAG, "key '%c' -> event %d%s", c >= ' ' && c < 0x7F ? c : '?', e,
                     locked ? " (locked, ignored)" : "");
            press(e);
        }
    }
}

static void serial_keys_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "serial keys: USB serial driver install failed");
        return;
    }
    usb_serial_jtag_vfs_use_driver();   // logs go through the driver too, so they don't collide
    xTaskCreate(serial_key_task, "serial_keys", 3072, NULL, 5, NULL);
    ESP_LOGW(TAG, "serial keys ON: arrows/a/d = left/right, Enter/space = select, "
                  "Backspace/b = back, h = home, A/D/S or held keys = holds, , . = previous / next track");
}
#endif

// Inputs with pull-ups. Pins armed for deep-sleep wake-up are handed back from the RTC
// domain first.
static void config_inputs(uint64_t mask)
{
    for (int pin = 0; pin < 64; pin++) {
        if ((mask & PIN_MASK(pin)) && rtc_gpio_is_valid_gpio(pin)) {
            rtc_gpio_deinit(pin);
        }
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

void buttons_init(void)
{
    uint64_t mask = PIN_MASK(PIN_LOCK);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (buttons[i].pin < EXP(0)) {
            mask |= PIN_MASK(buttons[i].pin);
        }
    }
    config_inputs(mask);
    expander_init();                                        // no expander: the 5-way stays silent
    vTaskDelay(pdMS_TO_TICKS(2));                           // let the pull-ups settle
    read_expander();

    // Start from the real state: something already held stays silent until released
    for (int i = 0; i < BUTTON_COUNT; i++) {
        buttons[i].pressed = is_low(buttons[i].pin);
        buttons[i].used = buttons[i].pressed;
    }
    locked = is_low(PIN_LOCK);
    ESP_LOGI(TAG, "controls %s", locked ? "locked" : "unlocked");

    events = xQueueCreate(8, sizeof(button_event_t));       // room for 8 unhandled presses
    xTaskCreate(button_task, "buttons", 2048, NULL, 5, NULL);  // 2 KB stack, medium priority
#if CONFIG_BUTTONS_SERIAL_KEYS
    serial_keys_start();
#endif
}

bool buttons_wait(button_event_t *event, TickType_t timeout)
{
    return xQueueReceive(events, event, timeout) == pdTRUE;
}

void buttons_post(button_event_t event)
{
    send(event);
}

bool buttons_locked(void)
{
    return locked;
}

// ---------- power on / off ----------

static bool power_button_down(void)
{
    return is_low(PIN_BACK) || is_low(PIN_KEY);
}

bool buttons_power_on_held(int hold_ms)
{
    config_inputs(POWER_PINS | PIN_MASK(PIN_LOCK));
    vTaskDelay(pdMS_TO_TICKS(2));

    if (is_low(PIN_LOCK)) {
        ESP_LOGI(TAG, "power button pressed while locked: staying off");
        return false;
    }
    int released_ms = 0;
    while (released_ms < DEBOUNCE_MS) {                     // let go for DEBOUNCE_MS = released
        if (esp_timer_get_time() / 1000 >= hold_ms) {
            return true;
        }
        released_ms = power_button_down() ? 0 : released_ms + POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    ESP_LOGI(TAG, "power button released early: staying off");
    return false;
}

void buttons_prepare_wake(void)
{
    config_inputs(POWER_PINS);
    for (int waited = 0; power_button_down() && waited < RELEASE_WAIT_MS; waited += POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    // Float the lock: its pull-up would draw current through the closed contact all the
    // time the device is off (the 5-way is on the expander, whose pins only conduct while
    // pressed)
    gpio_config_t off = {
        .pin_bit_mask = PIN_MASK(PIN_LOCK),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&off);

    // Either power button pulls its pin low to wake. The header button needs the chip's own
    // pull-up through deep sleep, which keeps the RTC peripherals powered (a few uA);
    // KEY has a pull-up on the board.
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(POWER_PINS, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
    rtc_gpio_pullup_en(PIN_BACK);
    rtc_gpio_pulldown_dis(PIN_BACK);
    rtc_gpio_pullup_dis(PIN_KEY);
    rtc_gpio_pulldown_dis(PIN_KEY);
}
