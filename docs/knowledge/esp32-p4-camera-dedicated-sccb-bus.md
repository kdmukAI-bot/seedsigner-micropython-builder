# A camera on its own SCCB bus needs the pins handed over, not the main I2C handle

## Symptom
With `CONFIG_CAMERA_SC2336=y` and `BOARD_HAS_CAMERA 1`, opening the camera fails at
sensor detection even though the sensor is demonstrably alive:

```
E (59318) i2c.master: I2C transaction unexpected nack detected
E (59338) sccb_i2c: s_sccb_i2c_transmit_receive_reg_a16v8(128): faled to transmit receive
E (59346) sc2336: Get sensor ID failed
E (59349) esp_video_init: failed to detect MIPI-CSI camera sensor with address=30
E (59362) pipeline_cam_csi: Failed to open /dev/video0
E (59366) cam_pipeline: Camera driver init failed
```

The display half of the pipeline initialises fine first, which makes this look like a
sensor/power/reset fault. It is not.

## What makes it confusing
The sensor answers perfectly from MicroPython at the same moment the driver cannot find
it — because the REPL probe names the bus explicitly:

```python
from machine import I2C, Pin
c = I2C(1, scl=Pin(34), sda=Pin(33), freq=100000)
c.scan()                                       # -> [0x30]
c.readfrom_mem(0x30, 0x3107, 2, addrsize=16)   # -> b'\xcb\x3a'  (SC2336)
```

So the rails are up, the sensor is out of reset, and the address is right. Only the
*bus the driver looks on* is wrong.

## Root cause
`esp_video`'s CSI config can either reuse a caller-supplied I2C bus handle or open its
own from pins:

```c
if (cfg->i2c_bus) {
    csi_config[0].sccb_config.init_sccb = false;
    csi_config[0].sccb_config.i2c_handle = cfg->i2c_bus;   /* reuse */
} else {
    csi_config[0].sccb_config.init_sccb = true;
    csi_config[0].sccb_config.i2c_config.port    = cfg->sccb_i2c_port;
    csi_config[0].sccb_config.i2c_config.sda_pin = cfg->sccb_sda_pin;
    csi_config[0].sccb_config.i2c_config.scl_pin = cfg->sccb_scl_pin;
}
```

`board_pipeline.c` used to fill in `.i2c_bus` unconditionally and never set the pin
fields. Every board in the fleet up to that point hung its sensor off the **same** I2C
bus as touch and the PMIC, so reusing the already-open main handle was always correct
and the self-init branch was dead code.

The Elecrow CrowPanel Advance 5" P4 is the first board where SCCB is a **separate bus**
(I2C1 on GPIO33/34, behind BSS138 level shifters, sensor side at 1.8 V). Handing over
the main bus handle makes the driver probe address 0x30 on the main bus pins — where
nothing answers — so detection NACKs.

Note the failure mode: a wrong-bus probe and an absent/unpowered sensor produce the
*same* NACK, which is why this reads as a hardware fault.

## Fix
Choose the branch from a compile-time board fact rather than always reusing the handle:

```c
#if BOARD_CAM_SCCB_I2C_PORT == BOARD_I2C_PORT
    .i2c_bus = (i2c_master_bus_handle_t)i2c_bus,
#else
    .i2c_bus = NULL,                 /* dedicated bus — esp_video opens it */
    .sccb_i2c_port = BOARD_CAM_SCCB_I2C_PORT,
    .sccb_sda_pin  = BOARD_PIN_CAM_SCCB_SDA,
    .sccb_scl_pin  = BOARD_PIN_CAM_SCCB_SCL,
#endif
```

Boards whose SCCB shares the main bus are unaffected — the comparison holds and they
keep the reuse path.

## Rule of thumb
`BOARD_CAM_SCCB_I2C_PORT` is not decoration. If it differs from `BOARD_I2C_PORT`, the
main bus handle must **not** be passed down, or the sensor is looked for on the wrong
pins. When a sensor NACKs, probe the declared bus from the REPL before suspecting
power or reset — an answer there localises the fault to the driver's bus selection.
