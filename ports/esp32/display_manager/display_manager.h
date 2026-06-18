#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "locale_loader.h"   /* ss_pack_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*display_manager_ui_callback_t)(void *ctx);

void init(void);
const char *run_screen(display_manager_ui_callback_t cb, void *ctx);

/* Switch the active i18n locale via the shared loader, wrapped in the LVGL-port
 * lock. `provider` acquires each pack file's bytes — on MicroPython the binding
 * supplies a provider backed by bytes read off the SD card in Python (the
 * ESP-IDF FAT stack can't be linked alongside MicroPython's own FAT VFS).
 * Returns true on full success; false if a pack is missing (the loader restores
 * the baked Western floor — a recoverable "fall back to English", not an error). */
bool dm_load_locale(const char *locale, ss_pack_provider_t provider, void *user);

/* Clear everything dm_load_locale installed and restore the baked Western floor. */
void dm_unload_locale(void);

#ifdef __cplusplus
}
#endif


#endif // DISPLAY_MANAGER_H
