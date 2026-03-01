# Set location of base MicroPython directory.
if(NOT MICROPY_DIR)
    get_filename_component(MICROPY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
endif()

# Set location of the ESP32 port directory.
if(NOT MICROPY_PORT_DIR)
    get_filename_component(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32 ABSOLUTE)
endif()

# Pull in external display stack component from EXTRA_COMPONENT_DIRS.
list(APPEND IDF_COMPONENTS display_manager)

# Force-exclude network stack components when building offline firmware.
if(MICROPY_DISABLE_NETWORK)
    set(EXCLUDE_COMPONENTS "${EXCLUDE_COMPONENTS};esp_wifi;esp_netif;lwip;bt;wpa_supplicant;esp_coex;tcp_transport;esp_http_client;esp_http_server;esp_https_server;esp_https_ota;wifi_provisioning;openthread;esp_local_ctrl;mqtt")
endif()
message(STATUS "MAIN_EXCLUDE_COMPONENTS=${EXCLUDE_COMPONENTS}")

include(${MICROPY_PORT_DIR}/esp32_common.cmake)
