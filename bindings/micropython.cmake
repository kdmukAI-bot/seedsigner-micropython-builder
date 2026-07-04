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
    # cUR's MicroPython binding (module `uUR`). Compiled here so its
    # MP_REGISTER_MODULE + MP_QSTR_* get QSTR-scanned; it calls the plain-C
    # ur_decoder_*/ur_encoder_* API in __idf_cUR (linked below).
    ${CMAKE_CURRENT_LIST_DIR}/../deps/cUR/uUR.c
)

target_include_directories(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/display_manager
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/camera_scanner
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/camera_entropy
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/board_common/src
    ${SEEDSIGNER_LVGL_SCREENS_DIR}/components/seedsigner
    # cUR repo root (uUR.c does #include "src/ur.h") + its src/ dir.
    ${CMAKE_CURRENT_LIST_DIR}/../deps/cUR
    ${CMAKE_CURRENT_LIST_DIR}/../deps/cUR/src
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
    __idf_cUR
)

target_link_libraries(usermod INTERFACE usermod_dm)
