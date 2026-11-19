# Script apps: the Lua host, the app API, installing apps, and the apps bundled with the firmware
idf_component_register(SRCS "script_host.c" "script_api.c" "script_json.c" "script_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES json                   # app_script.h uses cJSON
                       PRIV_REQUIRES app_core lua display battery buttons fatfs wear_levelling vfs
                                     esp_psram esp_timer esp_http_client mbedtls)

# Bundled apps: installed on first start, updated when the firmware brings a newer version.
# Sources live in the repository's apps/ folder, in the same layout as any app package.
# Each file is copied to a uniquely named file first ("clock_app.json"): embedding names
# its generated assembly after the file's base name, so two "app.json" would collide.
# configure_file also makes CMake re-run when a bundled file changes.
set(apps_dir "${CMAKE_CURRENT_LIST_DIR}/../../apps")
foreach(app clock weather)
    foreach(file app.json main.lua icon.bin)
        string(REPLACE "." "_" name "${app}_${file}")
        set(copy "${CMAKE_CURRENT_BINARY_DIR}/bundled/${app}_${file}")
        configure_file("${apps_dir}/${app}/${file}" "${copy}" COPYONLY)
        target_add_binary_data(${COMPONENT_LIB} "${copy}" BINARY RENAME_TO ${name})
    endforeach()
endforeach()
