#include "py/obj.h"
#include "py/runtime.h"

#include "display_manager.h"

static mp_obj_t mp_display_manager_init(void) {
    init();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(display_manager_init_obj, mp_display_manager_init);

static const mp_rom_map_elem_t display_manager_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_display_manager) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&display_manager_init_obj) },
};
static MP_DEFINE_CONST_DICT(display_manager_module_globals, display_manager_module_globals_table);

const mp_obj_module_t display_manager_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&display_manager_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_display_manager, display_manager_user_cmodule);
