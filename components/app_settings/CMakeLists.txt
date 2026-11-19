# Settings app: date and time by hand, screen rotation
idf_component_register(SRCS "settings_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_core
                       PRIV_REQUIRES display)
