#include "wifi.h"

#include <stdio.h>                      // snprintf()
#include <string.h>                     // strncpy(), strlen()
#include "esp_event.h"                  // event loop that Wi-Fi uses to report what happened
#include "esp_log.h"                    // logging macros
#include "esp_netif.h"                  // TCP/IP network interface (gets us an IP address)
#include "esp_wifi.h"                   // the Wi-Fi driver
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"      // lets app_main wait until a handler signals "done"
#include "sdkconfig.h"                  // CONFIG_WEATHER_WIFI_* from menuconfig

static const char *TAG = "wifi";

#define CONNECTED_BIT   BIT0            // set when we've received an IP address
#define FAILED_BIT      BIT1            // set when we've given up retrying
#define MAX_RETRIES     5               // failed attempts before wifi_connect() reports failure
#define CONNECT_TIMEOUT_MS 20000        // overall time limit for getting online

static EventGroupHandle_t wifi_events;  // holds CONNECTED_BIT / FAILED_BIT
static int retries;                     // reconnect attempts so far

// Called by the event loop whenever Wi-Fi or IP status changes
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();                                 // radio is up: start joining the network
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        retries++;                                          // dropped or failed to join
        if (retries == MAX_RETRIES) {
            xEventGroupSetBits(wifi_events, FAILED_BIT);    // tell wifi_connect() it isn't working...
        }
        ESP_LOGW(TAG, "disconnected, reconnecting (attempt %d)", retries);
        esp_wifi_connect();                                 // ...but keep trying in the background
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        retries = 0;                                        // fresh retry count for the next drop
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);     // online: tell wifi_connect()
    }
}

esp_err_t wifi_connect(void)
{
    wifi_events = xEventGroupCreate();                      // create the flags we'll wait on
    retries = 0;

    ESP_ERROR_CHECK(esp_netif_init());                      // start the TCP/IP stack
    ESP_ERROR_CHECK(esp_event_loop_create_default());       // start the system event loop
    esp_netif_create_default_wifi_sta();                    // network interface for "station" (client) mode

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();   // default driver buffers/settings
    ESP_ERROR_CHECK(esp_wifi_init(&init));                  // load the Wi-Fi driver

    // Route Wi-Fi events and the "got IP" event to on_event()
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t config = {0};                             // network name/password to join
    strncpy((char *)config.sta.ssid, CONFIG_WEATHER_WIFI_SSID, sizeof(config.sta.ssid));
    strncpy((char *)config.sta.password, CONFIG_WEATHER_WIFI_PASSWORD, sizeof(config.sta.password));
    // Accept open networks if no password was set, otherwise require at least WPA2
    config.sta.threshold.authmode = strlen(CONFIG_WEATHER_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // don't save credentials to flash
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));      // client mode (not an access point)
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());                      // triggers WIFI_EVENT_STA_START -> connect

    ESP_LOGI(TAG, "connecting to \"%s\"...", CONFIG_WEATHER_WIFI_SSID);
    // Block here until on_event() sets one of the bits, or the timeout passes
    EventBits_t bits = xEventGroupWaitBits(wifi_events, CONNECTED_BIT | FAILED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s", (bits & FAILED_BIT) ? "could not join network" : "timed out");
    return (bits & FAILED_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void wifi_disconnect(void)
{
    esp_wifi_disconnect();                                  // leave the network
    esp_wifi_stop();                                        // power the radio down
}

bool wifi_ip_string(char *out, size_t size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    snprintf(out, size, IPSTR, IP2STR(&ip.ip));
    return true;
}
