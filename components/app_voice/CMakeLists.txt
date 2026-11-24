# Voice Notes app: record from the INMP441 microphone to the microSD card, play back with the shared player
idf_component_register(SRCS "voice_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES audio buttons display player power sdcard esp_psram app_core)
