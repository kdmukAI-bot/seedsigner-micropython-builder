set(srcs "display_manager.cpp")
set(include_dirs ".")
set(component_requires "")

list(APPEND component_requires
    "board_common"
    "esp_lvgl_port"
    "lvgl"
    "seedsigner"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)

# Debug mode: enables dev-only features (boot splash text, etc.).
# Pass -DSEEDSIGNER_DEBUG=OFF for production builds.
option(SEEDSIGNER_DEBUG "Enable debug/dev features" ON)
if(SEEDSIGNER_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEDSIGNER_DEBUG=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEDSIGNER_DEBUG=0)
endif()
