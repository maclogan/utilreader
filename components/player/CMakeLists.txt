# The playback engine (shared by Music and Podcasts) and the shared list / Now Playing drawing
idf_component_register(SRCS "player.c" "media_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES musicdb app_core
                       PRIV_REQUIRES audio display esp_psram image)
