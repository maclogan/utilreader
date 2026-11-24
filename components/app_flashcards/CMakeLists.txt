# Flashcards: spaced-repetition review of the story app's saved words (opened from the Story app's menu)
idf_component_register(SRCS "flashcards_app.c" "flashcards_ui.c" "deck.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core buttons
                       PRIV_REQUIRES display review esp_psram)
