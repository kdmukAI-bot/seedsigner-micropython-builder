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
    # Native libsecp256k1 binding (top-level module `secp256k1`; embit imports it
    # on MicroPython). QSTR-scanned here; the EC math lives in __idf_esp-secp256k1
    # (linked below). Only the public secp256k1.h include dir is added below, so
    # the heavy internal headers stay out of the QSTR scan.
    ${CMAKE_CURRENT_LIST_DIR}/../deps/esp-secp256k1/mpy/modsecp256k1.c
    # hashlib extension (module `_hashlib_ext`): mbedtls-backed SHA-512 + PBKDF2.
    # A frozen hashlib.py merges these into the extensible built-in `hashlib`. The
    # mbedtls math lives in __idf_esp-hashlib-ext (linked below).
    ${CMAKE_CURRENT_LIST_DIR}/modhashlibext.c
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
    # secp256k1 public API only (modsecp256k1.c does #include "secp256k1.h").
    ${CMAKE_CURRENT_LIST_DIR}/../deps/esp-secp256k1/secp256k1-zkp/include
    # hashlib-ext plain-C API only (modhashlibext.c does #include "hashlib_ext.h");
    # the mbedtls headers stay inside the __idf_esp-hashlib-ext component.
    ${CMAKE_CURRENT_LIST_DIR}/../deps/esp-hashlib-ext
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
    __idf_esp-secp256k1
    # hashlib-ext: mbedtls-backed SHA-512 + PBKDF2 (plain-C lib; mbedtls stays
    # inside this component, out of the usermod QSTR-scan — same split as cUR/secp).
    __idf_esp-hashlib-ext
)

target_link_libraries(usermod INTERFACE usermod_dm)
