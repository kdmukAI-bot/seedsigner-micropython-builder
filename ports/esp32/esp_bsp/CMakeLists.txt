file(GLOB_RECURSE SRC_FILES  "*.c" "*.cpp")

# Strip Wi-Fi/network source from esp_bsp build.
list(FILTER SRC_FILES EXCLUDE REGEX ".*/bsp_wifi\.c$")

idf_component_register(SRCS ${SRC_FILES}
                    INCLUDE_DIRS "."
                    REQUIRES "esp_lcd" "driver" "esp_adc" "espressif__esp_lcd_axs15231b"  "fatfs" "nvs_flash" "XPowersLib" "esp32-camera" "espressif__esp_codec_dev")
