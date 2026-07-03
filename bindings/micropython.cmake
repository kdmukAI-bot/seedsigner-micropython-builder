add_library(usermod_dm INTERFACE)

# Single unified MicroPython module: seedsigner_lvgl_screens. It exposes init() +
# load_locale()/unload_locale() + the screens, matching the Pi Zero API name-for-
# name so the shared Python app needs no platform branching. The hardware/runtime
# C impl still lives in the display_manager component (linked below); there is no
# separate `display_manager` Python module.
target_sources(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modseedsigner_bindings.c
    ${CMAKE_CURRENT_LIST_DIR}/modcamera_scanner.c
    ${CMAKE_CURRENT_LIST_DIR}/modcamera_entropy.c
)

target_include_directories(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/display_manager
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/camera_scanner
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/camera_entropy
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/board_common/src
    ${SEEDSIGNER_LVGL_SCREENS_DIR}/components/seedsigner
)

# Link bindings against ESP-IDF component libs instead of compiling component C++
# sources in usermod qstr extraction. modcamera_scanner.c calls the plain-C
# cam_scanner_* API in __idf_camera_scanner (which owns the engine/LVGL/k_quirc
# headers, kept out of this QSTR-scan include set — same split as display_manager).
target_link_libraries(usermod_dm INTERFACE
    __idf_display_manager
    __idf_camera_scanner
    __idf_camera_entropy
    __idf_seedsigner
)

target_link_libraries(usermod INTERFACE usermod_dm)
