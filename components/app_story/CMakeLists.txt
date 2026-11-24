# Story app: daily Chinese story from Claude, reader with word lookup, saved words
idf_component_register(SRCS "story_app.c" "story.c" "story_ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core
                       PRIV_REQUIRES display review esp_http_client mbedtls json esp_psram app_flashcards)
