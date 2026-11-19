# The launcher: start-up, the main menu and the event loop. Apps live in components/.
idf_component_register(SRCS "main.c" "menu_ui.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES app_core display buttons net board_led battery
                                     app_weather app_clock app_story app_flashcards
                                     app_reader app_transfer books
                                     nvs_flash esp_netif)
