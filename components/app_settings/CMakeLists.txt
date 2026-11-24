# Settings app: date and time, screen rotation, the music / podcasts / e-books folders, formatting the card
idf_component_register(SRCS "settings_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES display sdcard books musicdb player podcasts)
