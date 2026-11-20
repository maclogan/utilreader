# Podcasts: feeds and episodes on the card, keep rules, RSS parser, the updater/downloader
idf_component_register(SRCS "podcasts.c" "podcasts_update.c" "rss.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core
                       PRIV_REQUIRES sdcard net fatfs esp_http_client esp-tls mbedtls esp_psram)
