# What apps are built on: the app interface, background worker, settings, files, local time
idf_component_register(SRCS "app.c" "app_registry.c" "worker.c" "settings.c" "storage.c" "local_time.c"
                       INCLUDE_DIRS "include"
                       REQUIRES buttons                # app.h uses button_event_t
                       PRIV_REQUIRES nvs_flash spiffs clock_chip)
