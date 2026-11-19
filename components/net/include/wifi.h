/*
 * Joins the Wi-Fi network set in menuconfig ("Weather Display" menu).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Connects and waits for an IP address. Returns ESP_OK once online,
// ESP_ERR_TIMEOUT / ESP_FAIL if the network can't be joined.
esp_err_t wifi_connect(void);

// The board's IP address as text (e.g. "192.168.1.174"). Returns false while offline.
bool wifi_ip_string(char *out, size_t size);

// Turns the radio off again (saves power before deep sleep)
void wifi_disconnect(void);
