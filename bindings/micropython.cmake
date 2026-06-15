add_library(usermod_dm INTERFACE)

target_sources(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/moddisplay_manager_bindings.c
    ${CMAKE_CURRENT_LIST_DIR}/modseedsigner_bindings.c
)

target_include_directories(usermod_dm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/display_manager
    ${CMAKE_CURRENT_LIST_DIR}/../ports/esp32/board_common/src
    ${SEEDSIGNER_LVGL_SCREENS_DIR}/components/seedsigner
)

# Link bindings against ESP-IDF component libs instead of compiling component C++
# sources in usermod qstr extraction.
target_link_libraries(usermod_dm INTERFACE
    __idf_display_manager
    __idf_seedsigner
)

target_link_libraries(usermod INTERFACE usermod_dm)
