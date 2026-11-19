# Transfer app: web interface on while it's open (book upload, saved words)
idf_component_register(SRCS "transfer_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES webui display net)
