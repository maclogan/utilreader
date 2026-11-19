# Weather app: Open-Meteo forecast, 7-day screen and day details
idf_component_register(SRCS "weather_app.c" "weather_api.c" "weather_ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core
                       PRIV_REQUIRES display esp_http_client mbedtls json)
