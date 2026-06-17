# ESP32-P4 microSD needs on-chip LDO power (machine.SDCard can't enable it)

## Symptom

On the Waveshare ESP32-P4 WiFi6 Touch LCD 4.3, `machine.SDCard(...)` *constructs*
without error, but the card is never actually accessed:

- `sd.ioctl(4, 0)` (block count) and `sd.ioctl(5, 0)` (block size) both return `-1`.
- `sd.writeblocks(0, buf)` + `readblocks` → data MISMATCH (writes don't stick).
- `vfs.VfsFat.mkfs(sd)` → `OSError(16)` (EBUSY).

Identical failure in 1-bit and 4-bit width, and at 20 MHz and 400 kHz — so it is
**not** signal integrity, width, or frequency. The card simply isn't responding.

## Root cause

On this board the microSD card's **VDD rail is supplied by the ESP32-P4 on-chip
LDO, channel 4 (`LDO_VO4`)**. The Waveshare BSP (`bsp_sdcard_mount`) powers it via:

```c
sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };  // LDO_VO4 → SDMMC IO
sd_pwr_ctrl_handle_t h = NULL;
sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &h);
host.pwr_ctrl_handle = h;           // attached to the SDMMC host
host.slot = SDMMC_HOST_SLOT_0;      // slot 0 = IOMUX, pins are automatic
```

MicroPython's `machine.SDCard` has **no API to set `host.pwr_ctrl_handle`**, so it
never enables `LDO_VO4`. The slot powers up at 0 V and the card never enumerates.
(The header literally documents: "set to `4` is the `LDO_VO4` is connected to power
the SDMMC IO" — `components/sdmmc/include/sd_pwr_ctrl_by_on_chip_ldo.h`.)

## Solution

Power `LDO_VO4` ourselves at boot, independently of `machine.SDCard`, and hold the
handle for the device's lifetime. Done in `display_manager.cpp` (`sd_power_on()`,
called from `seedsigner_board_startup`), guarded for `CONFIG_IDF_TARGET_ESP32P4`:

```c
#include "esp_ldo_regulator.h"                       // component: esp_hw_support
static esp_ldo_channel_handle_t s_sd_ldo = NULL;
esp_ldo_channel_config_t cfg = { .chan_id = 4, .voltage_mv = 3300 };
esp_ldo_acquire_channel(&cfg, &s_sd_ldo);            // VDD now 3.3 V
```

With the rail powered, MicroPython enumerates the card normally:

```python
sd = machine.SDCard(slot=0, width=4)   # slot 0 = IOMUX; pins automatic (43/44/39-42)
vfs.mount(vfs.VfsFat(sd), "/sd")
```

Notes:
- Use **slot 0** (IOMUX) to match the board wiring — no need to pass `sck/cmd/data`.
- The board has external pull-ups on CMD/DATA (BSP sets `slot_config.flags = 0`), so
  internal pull-ups are not the issue here — it was purely missing VDD.
- `LDO_VO3` is used for the MIPI-DSI PHY on this board; `LDO_VO4` is the SD rail.

## Source

Waveshare ESP32-P4 WiFi6 Touch LCD 4.3 BSP `bsp_sdcard_mount()`
(hardware-kb: `esp32-p4-touch-lcd-43/.../esp32_p4_wifi6_touch_lcd_4_3.c`), plus
ESP-IDF `sd_pwr_ctrl_by_on_chip_ldo.h` and `esp_ldo_regulator.h`. Diagnosed by
on-device `ioctl`/`writeblocks` probing showing block count `-1`.
