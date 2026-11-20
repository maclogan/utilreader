# The launcher: start-up, the main menu and the event loop. Apps live in components/.
idf_component_register(SRCS "main.c" "menu_ui.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES app_core display buttons net battery power
                                     app_story app_flashcards app_script
                                     app_reader app_music app_transfer app_settings books
                                     nvs_flash esp_netif)
