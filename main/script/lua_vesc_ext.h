#ifndef MAIN_SCRIPT_LUA_VESC_EXT_H_
#define MAIN_SCRIPT_LUA_VESC_EXT_H_

#include "script_lua.h"

// Register the VESC bindings as fields of the global `vesc` table.
void lua_vesc_ext_register(script_lua_t *s);

// UART and I2C, in lua_vesc_io.c.
void lua_vesc_io_register(script_lua_t *s);

/*
 * The bound names, for the `script_ext` terminal command and for
 * tools/script_ext_coverage.py. Returns a static array; count may be NULL.
 */
const char **lua_vesc_ext_names(int *count);

#endif /* MAIN_SCRIPT_LUA_VESC_EXT_H_ */
