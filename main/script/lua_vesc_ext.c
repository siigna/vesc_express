/*
	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * VESC bindings for the Lua engine.
 *
 * Naming rule, applied without exception so the two engines' surfaces can be
 * compared mechanically: the lisp extension `can-send-sid` becomes
 * `vesc.can_send_sid`. Lua has no hyphen in identifiers, and a table is a
 * better fit than 200 globals. tools/script_ext_coverage.py diffs the two
 * surfaces using exactly that rule, so an extension ported here shows up as
 * covered with no list to maintain by hand.
 *
 * This is a first set, not the full surface. Everything here is bound against
 * an API that was read in the source rather than assumed, and the coverage
 * tool reports honestly on what is still missing.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "commands.h"
#include "comm_can.h"
#include "flash_helper.h"
#include "adc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include <string.h>

// ---------------------------------------------------------------- system ---

static int l_systime(lua_State *L) {
	lua_pushinteger(L, (lua_Integer)xTaskGetTickCount());
	return 1;
}

static int l_secs_since(lua_State *L) {
	lua_Integer then = luaL_checkinteger(L, 1);
	TickType_t now = xTaskGetTickCount();
	lua_pushnumber(L, (lua_Number)((TickType_t)now - (TickType_t)then) /
			(lua_Number)configTICK_RATE_HZ);
	return 1;
}

/*
 * Yielding sleep. A script that wants to wait must not spin: the engine task
 * shares a core with the comms stack, and a busy wait here shows up as
 * dropped packets rather than as a slow script.
 */
static int l_sleep(lua_State *L) {
	lua_Number secs = luaL_checknumber(L, 1);
	if (secs < 0) {
		secs = 0;
	}
	// Round up, so sleep(0.0001) yields rather than becoming a no-op.
	TickType_t ticks = (TickType_t)((secs * (lua_Number)configTICK_RATE_HZ) + 0.999);
	vTaskDelay(ticks);
	return 0;
}

// ------------------------------------------------------------------ gpio ---

static int l_gpio_configure(lua_State *L) {
	int pin = (int)luaL_checkinteger(L, 1);
	const char *mode = luaL_optstring(L, 2, "in");

	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << (unsigned)pin,
		.intr_type = GPIO_INTR_DISABLE,
	};

	if (strcmp(mode, "out") == 0) {
		cfg.mode = GPIO_MODE_OUTPUT;
	} else if (strcmp(mode, "in-pu") == 0) {
		cfg.mode = GPIO_MODE_INPUT;
		cfg.pull_up_en = GPIO_PULLUP_ENABLE;
	} else if (strcmp(mode, "in-pd") == 0) {
		cfg.mode = GPIO_MODE_INPUT;
		cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
	} else if (strcmp(mode, "in") == 0) {
		cfg.mode = GPIO_MODE_INPUT;
	} else {
		return luaL_error(L, "gpio_configure: unknown mode '%s' "
				"(expected in, in-pu, in-pd or out)", mode);
	}

	lua_pushboolean(L, gpio_config(&cfg) == ESP_OK);
	return 1;
}

static int l_gpio_write(lua_State *L) {
	int pin = (int)luaL_checkinteger(L, 1);
	int level = lua_toboolean(L, 2);
	lua_pushboolean(L, gpio_set_level((gpio_num_t)pin, (uint32_t)level) == ESP_OK);
	return 1;
}

static int l_gpio_read(lua_State *L) {
	int pin = (int)luaL_checkinteger(L, 1);
	lua_pushboolean(L, gpio_get_level((gpio_num_t)pin));
	return 1;
}

// ------------------------------------------------------------------- can ---

/*
 * CAN payloads are Lua strings rather than tables of bytes: a string is
 * already a counted byte buffer, string.pack builds one in the format the
 * other end wants, and it avoids allocating a table per frame in a loop that
 * may run at a few hundred hertz.
 */
static const uint8_t *check_can_payload(lua_State *L, int idx, uint8_t *len_out) {
	size_t len = 0;
	const char *data = luaL_checklstring(L, idx, &len);
	if (len > 8) {
		luaL_error(L, "a CAN frame carries at most 8 bytes, got %d", (int)len);
		return NULL;
	}
	*len_out = (uint8_t)len;
	return (const uint8_t *)data;
}

static int l_can_send_sid(lua_State *L) {
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	uint8_t len = 0;
	const uint8_t *data = check_can_payload(L, 2, &len);
	comm_can_transmit_sid(id, data, len);
	return 0;
}

static int l_can_send_eid(lua_State *L) {
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	uint8_t len = 0;
	const uint8_t *data = check_can_payload(L, 2, &len);
	comm_can_transmit_eid(id, data, len);
	return 0;
}

static int l_can_ping(lua_State *L) {
	int id = (int)luaL_checkinteger(L, 1);
	if (id < 0 || id > 254) {
		return luaL_error(L, "can_ping: id %d is out of range", id);
	}
	HW_TYPE hw = HW_TYPE_VESC;
	bool ok = comm_can_ping((uint8_t)id, &hw);
	lua_pushboolean(L, ok);
	lua_pushinteger(L, (lua_Integer)hw);
	return 2;
}

// ---------------------------------------------------------------- eeprom ---

/*
 * The same 512-slot store the lisp engine exposes, and deliberately the same
 * slot numbering: a package ported from lisp to Lua keeps its stored settings
 * rather than silently starting from defaults.
 */
static int check_eeprom_addr(lua_State *L, int idx) {
	lua_Integer addr = luaL_checkinteger(L, idx);
	if (addr < 0 || addr >= EEPROM_VARS) {
		luaL_error(L, "eeprom address %d is outside 0..%d",
				(int)addr, EEPROM_VARS - 1);
	}
	return (int)addr;
}

static int l_eeprom_store_i(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	v.as_i32 = (int32_t)luaL_checkinteger(L, 2);
	lua_pushboolean(L, store_eeprom_var(&v, addr, 1));
	return 1;
}

static int l_eeprom_store_f(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	v.as_float = (float)luaL_checknumber(L, 2);
	lua_pushboolean(L, store_eeprom_var(&v, addr, 1));
	return 1;
}

/*
 * Returns nil for an unwritten slot rather than zero.
 *
 * This is the one place the lisp engine's behaviour is worth repeating
 * exactly, and it is a trap: on hardware an unwritten slot reads as nothing,
 * while a host test stub tends to hand back zero. A dash package that treated
 * the zero as a real setting worked in tests and fell over on a board.
 */
static int l_eeprom_read_i(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	if (!read_eeprom_var(&v, addr, 1)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, (lua_Integer)v.as_i32);
	return 1;
}

static int l_eeprom_read_f(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	if (!read_eeprom_var(&v, addr, 1)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)v.as_float);
	return 1;
}

static int l_eeprom_erase(lua_State *L) {
	lua_pushboolean(L, erase_eeprom_var(0, EEPROM_VARS));
	return 1;
}

// ------------------------------------------------------------------- adc ---

static int l_get_adc(lua_State *L) {
	int ch = (int)luaL_optinteger(L, 1, 0);
	lua_pushnumber(L, (lua_Number)adc_get_voltage((adc1_channel_t)ch));
	return 1;
}

// ------------------------------------------------------------ registration --

static const luaL_Reg vesc_fns[] = {
	{"systime", l_systime},
	{"secs_since", l_secs_since},
	{"sleep", l_sleep},

	{"gpio_configure", l_gpio_configure},
	{"gpio_write", l_gpio_write},
	{"gpio_read", l_gpio_read},

	{"can_send_sid", l_can_send_sid},
	{"can_send_eid", l_can_send_eid},
	{"can_ping", l_can_ping},

	{"eeprom_store_i", l_eeprom_store_i},
	{"eeprom_store_f", l_eeprom_store_f},
	{"eeprom_read_i", l_eeprom_read_i},
	{"eeprom_read_f", l_eeprom_read_f},
	{"eeprom_erase", l_eeprom_erase},

	{"get_adc", l_get_adc},

	{NULL, NULL},
};

void lua_vesc_ext_register(script_lua_t *s) {
	script_lua_register(s, vesc_fns);
}

const char **lua_vesc_ext_names(int *count) {
	static const char *names[sizeof(vesc_fns) / sizeof(vesc_fns[0])];
	int n = 0;
	for (const luaL_Reg *f = vesc_fns; f->name; f++) {
		names[n++] = f->name;
	}
	if (count) {
		*count = n;
	}
	return names;
}
