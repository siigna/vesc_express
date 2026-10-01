#ifndef MAIN_SCRIPT_LUA_VESC_EXT_H_
#define MAIN_SCRIPT_LUA_VESC_EXT_H_

#include "script_lua.h"

// Register the VESC bindings as fields of the global `vesc` table.
void lua_vesc_ext_register(script_lua_t *s);

// UART and I2C, in lua_vesc_io.c.
void lua_vesc_io_register(script_lua_t *s);

// Image buffers and drawing, in lua_vesc_disp.c. Host-testable: no driver
// dependencies.
void lua_vesc_disp_register(script_lua_t *s);

// Text, in lua_vesc_font.c. Adds buf:text() to the image metatable, so it
// must be registered after lua_vesc_disp_register.
void lua_vesc_font_register(script_lua_t *s);

// Config, in lua_vesc_conf.c. Target only.
void lua_vesc_conf_register(script_lua_t *s);

// Implemented in luaif.c, which owns the print prefix commands.c reads.
void luaif_set_print_prefix(const char *prefix);

// WiFi, in lua_vesc_wifi.c. Target only.
void lua_vesc_wifi_register(script_lua_t *s);

// BMS, in lua_vesc_bms.c. Target only.
void lua_vesc_bms_register(script_lua_t *s);

// Touch, in lua_vesc_touch.c. Target only.
void lua_vesc_touch_register(script_lua_t *s);

// Panel loaders, in lua_vesc_disp_load.c. Target only -- it names real
// drivers, which build against ESP-IDF.
void lua_vesc_disp_load_register(script_lua_t *s);

/*
 * The bound names, for the `script_ext` terminal command and for
 * tools/script_ext_coverage.py. Returns a static array; count may be NULL.
 */
const char **lua_vesc_ext_names(int *count);

#endif /* MAIN_SCRIPT_LUA_VESC_EXT_H_ */
