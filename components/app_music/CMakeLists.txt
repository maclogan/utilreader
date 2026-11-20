# Music app: iPod-style library (playlists, artists, albums, songs) from the microSD card,
# decoded with esp_audio_codec, out through the TLV320DAC3100 (components/audio)
idf_component_register(SRCS "music_app.c" "player.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core
                       PRIV_REQUIRES musicdb audio sdcard display buttons power esp_psram esp_hw_support)
