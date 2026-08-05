#define MICROPY_HW_BOARD_NAME               "Elecrow CrowPanel Advance 5in P4"
#define MICROPY_HW_MCU_NAME                 "ESP32P4"

// Keep UART REPL enabled for bring-up/fallback. This board's only USB serial is
// an external CH340K bridge on UART0 (it enumerates as /dev/ttyUSB*, unlike the
// P4 boards that expose the SoC's native USB-Serial-JTAG on /dev/ttyACM*), so
// the UART REPL is the ONLY console here, not a fallback.
#define MICROPY_HW_ENABLE_UART_REPL         (1)

// Default I2C pins for MicroPython machine.I2C(0) — the main bus, shared by the
// touch controller and the STC8 companion MCU. The camera's SCCB is a separate
// bus (GPIO33/34) and is not exposed here.
#define MICROPY_HW_I2C0_SCL                 (46)
#define MICROPY_HW_I2C0_SDA                 (45)

// Disable networking (P4 has no built-in WiFi/BT;
// external WiFi6 module is unused for SeedSigner).
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_NETWORK_WLAN             (0)
#define MICROPY_PY_NETWORK_LAN              (0)
#define MICROPY_PY_NETWORK_PPP_LWIP         (0)
#define MICROPY_PY_SOCKET                   (0)

// Disable Bluetooth and ESP-NOW support.
#define MICROPY_PY_BLUETOOTH               (0)
#define MICROPY_BLUETOOTH_NIMBLE           (0)
#define MICROPY_PY_ESPNOW                  (0)

// Initialize display at C-level boot (before REPL).
extern void seedsigner_board_startup(void);
#define MICROPY_BOARD_STARTUP seedsigner_board_startup
