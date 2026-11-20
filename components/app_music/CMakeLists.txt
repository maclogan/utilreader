# Music app: audio files from the microSD card, decoded with esp_audio_codec, out through
# the TLV320DAC3100 (components/audio)
idf_component_register(SRCS "music_app.c" "player.c" "library.c" "media_info.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core
                       PRIV_REQUIRES audio sdcard display buttons power esp_psram)
