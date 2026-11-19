# Script apps: the Lua host, the app API, installing apps, and the apps bundled with the firmware
idf_component_register(SRCS "script_host.c" "script_api.c"
                       INCLUDE_DIRS "include"
                       REQUIRES json                   # app_script.h uses cJSON
                       PRIV_REQUIRES app_core lua display battery buttons fatfs wear_levelling vfs
                                     esp_psram)

# Bundled apps: installed on first start, updated when the firmware brings a newer version.
# Sources live in the repository's apps/ folder, in the same layout as any app package.
# RENAME_TO gives each file a unique symbol (they'd all be "app_json" otherwise).
set(apps_dir "${CMAKE_CURRENT_LIST_DIR}/../../apps")
foreach(app clock)
    target_add_binary_data(${COMPONENT_LIB} "${apps_dir}/${app}/app.json" BINARY RENAME_TO ${app}_app_json)
    target_add_binary_data(${COMPONENT_LIB} "${apps_dir}/${app}/main.lua" BINARY RENAME_TO ${app}_main_lua)
    target_add_binary_data(${COMPONENT_LIB} "${apps_dir}/${app}/icon.bin" BINARY RENAME_TO ${app}_icon_bin)
endforeach()
