set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.p4
    boards/ELECROW_CROWPANEL_ADV_P4_50/sdkconfig.board
)

# Insert board hardware sdkconfig from esp-board-common before sdkconfig.board,
# so MicroPython-specific settings in sdkconfig.board take precedence.
if(DEFINED BOARD_CONFIG_DIR AND EXISTS "${BOARD_CONFIG_DIR}/sdkconfig.defaults")
    list(INSERT SDKCONFIG_DEFAULTS 2 "${BOARD_CONFIG_DIR}/sdkconfig.defaults")
endif()

# Freeze the SeedSigner app's required stdlib modules (logging + hmac, from
# micropython-lib) into the firmware so they need not be vendored to /lib.
set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)
