# The music library on the microSD card: tags (cached), artists, albums, playlists.
# Shared by the Music app and the web interface.
idf_component_register(SRCS "musicdb.c" "playlists.c" "media_info.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES sdcard esp_psram)
