# camera_scanner — builder-side QR-scan composition (pipeline + overlay +
# scan_coordinator) exposed to MicroPython. Mirrors the display_manager component:
# the bindings link __idf_camera_scanner and call its plain-C cam_scanner_* API,
# keeping the engine/LVGL/k_quirc headers out of the QSTR-scan include set.
set(srcs "camera_scanner.cpp")
set(include_dirs ".")

set(component_requires
    "board_common"        # scan_coordinator + board_pipeline + board_i2c
    "esp-camera-pipeline" # cam_pipeline_create / esp_cam_pipeline.h
    "cam_pipeline_qr"     # cam_pipeline_qr.h / k_quirc types
    "seedsigner"          # camera_preview_overlay (LVGL screen widgets)
    "esp_lvgl_port"       # lvgl_port_lock
    "lvgl"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

# camera_preview_overlay.cpp (in the seedsigner component) is C++ with exceptions;
# match display_manager so this component's C++ TU links consistently.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)

# 480-decode experiment (partition-mode boards): SS_CAM_DECODE_480=1 in the
# environment makes QR-scan sessions run the pipeline + decoders at 480x480
# while the display sink decimates 3->2 back to the 320 panel square. Env-var
# (not -D cache) so both of ESP-IDF's evaluation passes agree — same pattern
# as BOARD_CONFIG_DIR. Default off = deployed behavior.
if("$ENV{SS_CAM_DECODE_480}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_CAMERA_DECODE_480=1)
    message(STATUS "camera_scanner: 480-decode experiment ENABLED (SS_CAM_DECODE_480=1)")
endif()
