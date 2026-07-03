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
