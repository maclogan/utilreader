# net: Wi-Fi

Joins the 2.4 GHz network set in menuconfig (*Wi-Fi → Wi-Fi network name / password*;
the options are still called `CONFIG_WEATHER_WIFI_*`, from when they sat in the weather menu). The ESP32-S3 can't use 5 GHz networks.

```c
esp_err_t err = wifi_connect();   // blocks until online, up to 20 s
```

- Returns `ESP_OK` once the board has an IP address, `ESP_FAIL` after 5 failed attempts,
  or `ESP_ERR_TIMEOUT` after 20 s. **Either way the driver keeps reconnecting in the
  background**, so later downloads work as soon as the network is back. A few
  `disconnected, reconnecting (attempt N)` lines at power-on are normal.
- Credentials are kept in RAM only (`WIFI_STORAGE_RAM`), never written to flash by the driver.
- Open networks work if the password is left empty; otherwise WPA2 or better is required.
- `wifi_disconnect()` turns the radio off (for battery-powered use; not used now).

## How it works

`wifi_connect()` starts the TCP/IP stack and default event loop, creates the station
interface, registers `on_event()` for Wi-Fi and IP events, then waits on an event group:

```
WIFI_EVENT_STA_START          -> esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED   -> count the attempt, connect again
                                 (after 5 attempts also tell wifi_connect() it failed)
IP_EVENT_STA_GOT_IP           -> reset the count, tell wifi_connect() we're online
```

Making HTTPS requests is up to each app (see `app_story/story.c` and, for Lua apps,
`app_script/script_net.c`, which use `esp_http_client` with the built-in certificate bundle).
Run them on the worker (`app_core/worker.h`), never in the main task.
