# Clock app: big digits, redrawn every minute
idf_component_register(SRCS "clock_app.c" "clock_ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_core
                       PRIV_REQUIRES display)
