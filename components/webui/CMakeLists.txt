# Web interface: book upload/delete and the saved-words list, served on port 80
idf_component_register(SRCS "webui.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server books review net json esp_psram buttons esp_timer
                       EMBED_TXTFILES "index.html")
