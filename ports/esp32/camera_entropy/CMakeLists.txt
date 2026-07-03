# camera_entropy — builder-side image-entropy composition (pipeline + entropy
# consumer) exposed to MicroPython. Mirrors camera_scanner: the bindings link
# __idf_camera_entropy and call its plain-C cam_entropy_* API, keeping the
# engine/LVGL headers out of the QSTR-scan include set. No overlay, so (unlike
# camera_scanner) it does not require the seedsigner LVGL-screens component.
set(srcs "camera_entropy.cpp")
set(include_dirs ".")

set(component_requires
    "board_common"           # board_pipeline + board_i2c + board_config
    "esp-camera-pipeline"    # cam_pipeline_create / esp_cam_pipeline.h
    "cam_pipeline_entropy"   # cam_pipeline_entropy.h — the SHA-256 frame consumer
    "esp_lvgl_port"          # lvgl_port_lock
    "lvgl"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

# This component's C++ TU uses exceptions transitively (board/LVGL headers); match
# camera_scanner / display_manager so it links consistently.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
