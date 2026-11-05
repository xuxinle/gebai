// 由 tools/micropython_genhdr.py 生成：从 MP_REGISTER_MODULE 抽取
extern const mp_obj_module_t mp_module_builtins;
extern const mp_obj_module_t mp_module_math;
extern const mp_obj_module_t mp_module_sys;
extern const mp_obj_module_t mp_module___main__;
#define MICROPY_REGISTERED_MODULES \
    { MP_ROM_QSTR(MP_QSTR_builtins), MP_ROM_PTR(&mp_module_builtins) }, \
    { MP_ROM_QSTR(MP_QSTR_math), MP_ROM_PTR(&mp_module_math) }, \
    { MP_ROM_QSTR(MP_QSTR_sys), MP_ROM_PTR(&mp_module_sys) }, \
    { MP_ROM_QSTR(MP_QSTR___main__), MP_ROM_PTR(&mp_module___main__) }, \
    { MP_ROM_QSTR(MP_QSTRnull), MP_ROM_PTR(NULL) }
#define MICROPY_REGISTERED_EXTENSIBLE_MODULES \
    { MP_ROM_QSTR(MP_QSTRnull), MP_ROM_PTR(NULL) }
// 共 4 个模块
