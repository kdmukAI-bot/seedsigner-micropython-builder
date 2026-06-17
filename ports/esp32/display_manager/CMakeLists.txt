set(srcs "display_manager.cpp")
set(include_dirs ".")
set(component_requires "")

list(APPEND component_requires
    "board_common"
    "esp_lvgl_port"
    "lvgl"
    "seedsigner"
    "esp_hw_support"   # esp_ldo_regulator.h — power the P4 SD card VDD rail
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)

# Display resolution profile: hardware builds target a single display height to
# avoid compiling unused font sets.  The seedsigner component checks for at least
# one SUPPORT_DISPLAY_HEIGHT_* flag at compile time (gui_constants.h).  Setting
# it PUBLIC here propagates to all components that REQUIRE seedsigner.
# SEEDSIGNER_DISPLAY_HEIGHT is passed from build_firmware.sh via CMAKE_ARGS.
if(NOT DEFINED SEEDSIGNER_DISPLAY_HEIGHT)
    set(SEEDSIGNER_DISPLAY_HEIGHT 320)
endif()
target_compile_definitions(__idf_seedsigner PUBLIC SUPPORT_DISPLAY_HEIGHT_${SEEDSIGNER_DISPLAY_HEIGHT})

# Debug mode: enables dev-only features (boot splash text, etc.).
# Pass -DSEEDSIGNER_DEBUG=OFF for production builds.
option(SEEDSIGNER_DEBUG "Enable debug/dev features" ON)
if(SEEDSIGNER_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEDSIGNER_DEBUG=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEDSIGNER_DEBUG=0)
endif()
