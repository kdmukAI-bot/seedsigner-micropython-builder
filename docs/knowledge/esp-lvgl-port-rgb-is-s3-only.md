# `lvgl_port_add_disp_rgb()` is ESP32-S3-only — it returns NULL on the ESP32-P4

## Symptom

You wire up a parallel-RGB panel on an ESP32-P4, call
`lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg)` exactly as the header and the
examples show, and get back `NULL`. The build is clean — the symbol exists, the
struct fields all exist, nothing warns. If you `assert()` on the return value you
abort at boot; if you don't, you carry a NULL display handle into LVGL.

## Cause

The function's body is target-gated. From
`espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c`:

```c
lv_display_t *lvgl_port_add_disp_rgb(const lvgl_port_display_cfg_t *disp_cfg,
                                     const lvgl_port_display_rgb_cfg_t *rgb_cfg)
{
    lvgl_port_lock(0);
    ...
    lv_disp_t *disp = lvgl_port_add_disp_priv(disp_cfg, &priv_cfg);
    if (disp != NULL) {
        ...
        disp_ctx->disp_type = LVGL_PORT_DISP_TYPE_RGB;
#if (CONFIG_IDF_TARGET_ESP32S3 && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
        /* register vsync / bounce-frame callbacks */
#else
        ESP_RETURN_ON_FALSE(false, NULL, TAG,
                            "RGB is supported only on ESP32S3 and from IDF 5.0!");
#endif
    }
    ...
}
```

Two things make this worse than a plain "unsupported" return:

1. **The display is already created before the bail-out.** `lvgl_port_add_disp_priv()`
   runs first and registers a working display with LVGL. The function then returns
   NULL, so the *caller* loses the handle while the display exists. Code that
   tolerates NULL can appear to work (LVGL falls back to the default display) and
   then break in confusing ways later — e.g. `lvgl_port_add_touch()` binds to the
   wrong display.
2. **`ESP_RETURN_ON_FALSE` returns without unlocking.** It skips the
   `lvgl_port_unlock()` at the end of the function.

There is a second, independent P4 bug in the same file. The tear-avoidance path
assumes P4 implies MIPI-DSI, so it fetches framebuffers with the **DSI** accessor
even when the panel is RGB:

```c
if (priv_cfg && priv_cfg->avoid_tearing) {
#if CONFIG_IDF_TARGET_ESP32S3 && ...
        esp_lcd_rgb_panel_get_frame_buffer(disp_cfg->panel_handle, 2, &buf1, &buf2);
#elif CONFIG_IDF_TARGET_ESP32P4 && ...
        esp_lcd_dpi_panel_get_frame_buffer(disp_cfg->panel_handle, 2, &buf1, &buf2);
#endif
```

So on a P4 + RGB panel, `avoid_tearing = 1` cannot work either.

## Versions checked

- `espressif/esp_lvgl_port` **2.7.2** (our pin)
- `espressif/esp_lvgl_port` **2.6.2** (bundled in the Elecrow CrowPanel P4 factory sources)

Both identical in this respect. The board vendor calls `lvgl_port_add_disp_rgb()`
and their own firmware gets NULL back from it — their code logs "LVGL rgb port add
fail" and carries on, working by accident because the display object was created
anyway.

### Likely fixed in 2.8.0 — the upgrade path out of this

`docs/camera-pipeline-research-5.md` (finding F3) records that **esp_lvgl_port
2.8.0** added *"Supported RGB/MIPI-DSI interfaces for chips by `SOC_*`"*, an
ESP32-P4 RGB example, and RGB565 swapped colour — citing the Espressif Component
Registry changelog. Gating on `SOC_LCD_RGB_SUPPORTED` instead of
`CONFIG_IDF_TARGET_ESP32S3` is exactly the fix for what is described above, and
the P4 sets that SOC flag.

**Not verified here** — this is read off that changelog note, not tested. If the
hand-rolled path below ever becomes a maintenance burden, bumping the component
and re-testing `lvgl_port_add_disp_rgb()` on a P4 is the thing to try first. Note
that a bump also affects every other board's display path, so it is not a local
change.

This is not a P4 hardware limitation: the P4 has an RGB/LCD_CAM peripheral
(`SOC_LCD_RGB_SUPPORTED`) and `esp_lcd_new_rgb_panel()` drives it fine. Only the
LVGL port wrapper is gated.

## What to do instead

Create the LVGL display directly — the same approach `board_init.c` already uses
for the ST7701 landscape path — and let the flush copy into the framebuffer:

```c
lvgl_port_lock(0);
lv_display_t *disp = lv_display_create(hres, vres);
lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_flush_cb(disp, rgb_flush_cb);
lvgl_port_unlock();

static void rgb_flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_display_flush_ready(d);          /* synchronous — see below */
}
```

**Why the flush can report ready immediately.** For an RGB panel there is no bus
transaction to wait on: the peripheral scans a framebuffer out continuously. When
the source buffer is not itself one of the framebuffers,
`rgb_panel_draw_bitmap()` does a **CPU copy into `fbs[cur_fb_index]`** — the one
currently being displayed — and returns. Nothing is queued.

That last detail also means **extra framebuffers are wasted unless you switch
them yourself**: `num_fbs = 2` with a partial-copy flush leaves the second buffer
untouched, because `cur_fb_index` only moves when something calls
`esp_lcd_rgb_panel_switch_buffer()`. Either keep `num_fbs = 1`, or go all the way
to the tear-free arrangement (render straight into the framebuffers, switch on
VSYNC) and pay for two.

## Trade-off accepted

Copying a partial region into the live framebuffer can tear. That is the vendor's
configuration and it is fine for bring-up. The tear-free upgrade is a separate,
larger change (framebuffer-direct rendering + a VSYNC callback + buffer switching),
not a flag flip.
