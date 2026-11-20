# Podcasts app: episodes downloaded from feeds, played with the shared player engine
idf_component_register(SRCS "podcasts_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES podcasts player audio sdcard display buttons power esp_psram)
