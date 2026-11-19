#include "buttons.h"

#include "driver/gpio.h"                // gpio_config(), gpio_get_level()
#include "esp_err.h"                    // ESP_ERROR_CHECK()
#include "freertos/queue.h"             // queue that carries events to app_main
#include "freertos/task.h"              // xTaskCreate(), vTaskDelay()

// Elecrow CrowPanel ESP32 E-Paper 2.13" (from Elecrow's schematic). All inputs connect the
// pin to GND when pressed, with pull-up resistors on the board.
#define PIN_WHEEL_UP    6               // IO6_UP:   thumbwheel rolled up
#define PIN_WHEEL_DOWN  4               // IO4_DOWN: thumbwheel rolled down
#define PIN_WHEEL_PRESS 5               // IO5_CONF: thumbwheel pressed in
#define PIN_EXIT        1               // IO1_EXIT: EXIT button
#define PIN_MENU        2               // IO2_MENU: MENU / HOME button

#define PRESSED_LEVEL   0               // pressed = pulled to GND

#define POLL_MS         10              // how often the inputs are checked
#define DEBOUNCE_MS     30              // a reading must hold steady this long to count (filters contact bounce)
#define LONG_PRESS_MS   500             // holding this long = a "hold" gesture

#define NO_HOLD         ((button_event_t)-1)

static QueueHandle_t events;            // button events waiting to be handled by app_main

// One physical input and the events it produces
typedef struct {
    int pin;                            // GPIO number
    button_event_t tap;                 // sent on a short press
    button_event_t hold;                // sent once after LONG_PRESS_MS, or NO_HOLD
    bool pressed;                       // current debounced state
    int change_ms;                      // how long the raw reading has disagreed with `pressed`
    int held_ms;                        // how long it has been held down
    bool used;                          // this press already sent its event: nothing on release
} button_t;

// Inputs without a hold gesture send their tap as soon as they're pressed (snappier).
// Inputs with one send the tap on release, so a press that becomes a hold doesn't also tap.
static button_t buttons[] = {
    {.pin = PIN_WHEEL_UP,    .tap = BUTTON_LEFT,   .hold = BUTTON_LEFT_HOLD},
    {.pin = PIN_WHEEL_DOWN,  .tap = BUTTON_RIGHT,  .hold = BUTTON_RIGHT_HOLD},
    {.pin = PIN_WHEEL_PRESS, .tap = BUTTON_SELECT, .hold = NO_HOLD},
    {.pin = PIN_EXIT,        .tap = BUTTON_BACK,   .hold = NO_HOLD},
    {.pin = PIN_MENU,        .tap = BUTTON_HOME,   .hold = NO_HOLD},
};
#define BUTTON_COUNT ((int)(sizeof(buttons) / sizeof(buttons[0])))

static void send(button_event_t e)
{
    xQueueSend(events, &e, 0);          // 0 = don't wait if the queue is full (drop the press)
}

// Debounces one input and sends its events
static void update_button(button_t *b)
{
    bool raw = gpio_get_level(b->pin) == PRESSED_LEVEL;    // true while physically pressed

    if (raw == b->pressed) {
        b->change_ms = 0;                                   // reading agrees: no change pending
    } else if ((b->change_ms += POLL_MS) >= DEBOUNCE_MS) {  // new reading has lasted long enough
        b->pressed = raw;                                   // accept the new state
        b->change_ms = 0;
        if (raw) {                                          // just pressed: start a fresh press
            b->held_ms = 0;
            b->used = false;
            if (b->hold == NO_HOLD) {
                send(b->tap);                               // nothing to wait for
                b->used = true;
            }
        } else if (!b->used) {                              // just released, and it wasn't a hold
            send(b->tap);
        }
    }

    if (b->pressed) {
        b->held_ms += POLL_MS;
        if (!b->used && b->hold != NO_HOLD && b->held_ms >= LONG_PRESS_MS) {
            send(b->hold);                                  // fires while still held
            b->used = true;                                 // once per press, and no tap on release
        }
    }
}

// Runs forever: reads the inputs every POLL_MS
static void button_task(void *arg)
{
    while (1) {
        for (int i = 0; i < BUTTON_COUNT; i++) {
            update_button(&buttons[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        mask |= 1ULL << buttons[i].pin;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,       // the board has pull-ups too; harmless together
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    events = xQueueCreate(8, sizeof(button_event_t));  // room for 8 unhandled presses
    xTaskCreate(button_task, "buttons", 2048, NULL, 5, NULL);  // 2 KB stack, medium priority
}

bool buttons_wait(button_event_t *event, TickType_t timeout)
{
    return xQueueReceive(events, event, timeout) == pdTRUE;
}

void buttons_post(button_event_t event)
{
    send(event);
}
