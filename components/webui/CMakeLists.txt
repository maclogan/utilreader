# Web interface: book upload/delete, script apps, the saved-words list, playlists, served on port 80
idf_component_register(SRCS "webui.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server books review net json esp_psram buttons esp_timer app_script app_core musicdb
                       EMBED_TXTFILES "index.html")
