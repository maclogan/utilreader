# Transfer app: web interface on while it's open (books, playlists, podcasts, saved words);
# podcast feeds are updated and episodes downloaded while it's open
idf_component_register(SRCS "transfer_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES webui display net podcasts buttons)
