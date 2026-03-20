#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*display_manager_ui_callback_t)(void *ctx);

void init(void);
const char *run_screen(display_manager_ui_callback_t cb, void *ctx);

#ifdef __cplusplus
}
#endif


#endif // DISPLAY_MANAGER_H
