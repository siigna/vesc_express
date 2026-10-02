/*
	Copyright 2025 Stephen Bouche

	This file is part of the ESCargot firmware.

	The ESCargot firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The ESCargot firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * BMS bindings for the Lua engine.
 *
 * These cover the generic BMS values -- the pack state the firmware collects
 * from CAN, which is what a dash wants. They are not the whole of what lisp
 * calls bms-something: twenty of those extensions belong to one BMS board's
 * own glue (hwconf/trampa/bms_rb), registering register-level access to its
 * own hardware. Those are board code rather than bindings and would need that
 * board ported, not wrapped.
 *
 * Deliberately absent: bms-st. The lisp version starts a self-test over CAN
 * and then parks its own context with
 * lbm_block_ctx_from_extension_timeout(10.0), resuming when the reply lands.
 * Lua has no equivalent here -- it would need the engine to yield a coroutine
 * and resume it from the comms task -- and a binding that waited ten seconds
 * inside a C call would stall the engine instead. Worth doing with async
 * support; not worth faking.
 *
 * Keys are the lisp names with the bms- prefix dropped, since the function
 * already says BMS. Both spellings of a separator are accepted, so a key
 * copied out of a lisp script ("v-cell-min") works as well as the Lua-looking
 * one ("v_cell_min").
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "bms.h"
#include "datatypes.h"
#include "commands.h"

// utils.h uses xTaskGetTickCount and portTICK_PERIOD_MS in UTILS_AGE_S, which
// it does not include itself.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "utils.h"

#include <stddef.h>
#include <string.h>

typedef enum {
	BV_FLOAT,
	BV_INT,
	BV_STATUS,		// char[BMS_STATUS_LEN], read and written as a string
	BV_AGE,			// Computed from update_time, so read only
	BV_CELL_V,		// float[], indexed, bounded by cell_num
	BV_BAL,			// bool[], indexed, bounded by cell_num
	BV_TEMP_ADC,	// float[], indexed, bounded by temp_adc_num
} bms_val_type_t;

typedef struct {
	const char *name;
	bms_val_type_t type;
	size_t offset;
} bms_val_t;

/*
 * One row per key, rather than the thirty-branch if-chain the lisp side uses.
 * The offsets are into bms_values; a mistake here is a wrong field rather
 * than a crash, so the types are what keep it honest.
 */
static const bms_val_t bms_vals[] = {
	{"v_tot",				BV_FLOAT,		offsetof(bms_values, v_tot)},
	{"v_charge",			BV_FLOAT,		offsetof(bms_values, v_charge)},
	{"i_in",				BV_FLOAT,		offsetof(bms_values, i_in)},
	{"i_in_ic",				BV_FLOAT,		offsetof(bms_values, i_in_ic)},
	{"ah_cnt",				BV_FLOAT,		offsetof(bms_values, ah_cnt)},
	{"wh_cnt",				BV_FLOAT,		offsetof(bms_values, wh_cnt)},
	{"cell_num",			BV_INT,			offsetof(bms_values, cell_num)},
	{"v_cell",				BV_CELL_V,		offsetof(bms_values, v_cell)},
	{"bal_state",			BV_BAL,			offsetof(bms_values, bal_state)},
	{"temp_adc_num",		BV_INT,			offsetof(bms_values, temp_adc_num)},
	{"temps_adc",			BV_TEMP_ADC,	offsetof(bms_values, temps_adc)},
	{"temp_ic",				BV_FLOAT,		offsetof(bms_values, temp_ic)},
	{"temp_hum",			BV_FLOAT,		offsetof(bms_values, temp_hum)},
	{"hum",					BV_FLOAT,		offsetof(bms_values, hum)},
	{"pres",				BV_FLOAT,		offsetof(bms_values, pressure)},
	// The lisp symbol is bms-temp-cell-max while the field is temp_max_cell.
	// Keeping the lisp spelling, because that is what scripts and the docs
	// use.
	{"temp_cell_max",		BV_FLOAT,		offsetof(bms_values, temp_max_cell)},
	{"v_cell_min",			BV_FLOAT,		offsetof(bms_values, v_cell_min)},
	{"v_cell_max",			BV_FLOAT,		offsetof(bms_values, v_cell_max)},
	{"soc",					BV_FLOAT,		offsetof(bms_values, soc)},
	{"soh",					BV_FLOAT,		offsetof(bms_values, soh)},
	{"can_id",				BV_INT,			offsetof(bms_values, can_id)},
	{"ah_cnt_chg_total",	BV_FLOAT,		offsetof(bms_values, ah_cnt_chg_total)},
	{"wh_cnt_chg_total",	BV_FLOAT,		offsetof(bms_values, wh_cnt_chg_total)},
	{"ah_cnt_dis_total",	BV_FLOAT,		offsetof(bms_values, ah_cnt_dis_total)},
	{"wh_cnt_dis_total",	BV_FLOAT,		offsetof(bms_values, wh_cnt_dis_total)},
	{"msg_age",				BV_AGE,			offsetof(bms_values, update_time)},
	{"chg_allowed",			BV_INT,			offsetof(bms_values, is_charge_allowed)},
	{"data_version",		BV_INT,			offsetof(bms_values, data_version)},
	{"status",				BV_STATUS,		offsetof(bms_values, status)},
	// Not reachable from lisp, which never got symbols for them, but they are
	// in the same struct and a dash wants both.
	{"is_charging",			BV_INT,			offsetof(bms_values, is_charging)},
	{"is_balancing",		BV_INT,			offsetof(bms_values, is_balancing)},
};

#define BMS_VAL_CNT	(sizeof(bms_vals) / sizeof(bms_vals[0]))

// '-' and '_' are the same separator here, so a key lifted from a lisp
// script works unchanged.
static bool key_eq(const char *key, const char *name) {
	while (*key && *name) {
		char a = *key == '-' ? '_' : *key;
		if (a != *name) {
			return false;
		}
		key++;
		name++;
	}

	return *key == '\0' && *name == '\0';
}

static const bms_val_t *bms_val_find(const char *key) {
	for (size_t i = 0;i < BMS_VAL_CNT;i++) {
		if (key_eq(key, bms_vals[i].name)) {
			return &bms_vals[i];
		}
	}

	return NULL;
}

static void *field_ptr(const bms_val_t *v) {
	return (void *)((char *)bms_get_values() + v->offset);
}

// Index for one of the array keys, bounded by the count the BMS reported
// rather than by the array size: a cell the pack does not have holds stale
// data, and returning it as a reading would be worse than an error.
static int array_index(lua_State *L, const bms_val_t *v, int arg) {
	volatile bms_values *val = bms_get_values();
	int limit = (v->type == BV_TEMP_ADC) ? val->temp_adc_num : val->cell_num;
	int idx = (int)luaL_checkinteger(L, arg);

	if (idx < 0 || idx >= limit) {
		luaL_error(L, "bms index %d is outside 0..%d", idx, limit - 1);
	}

	return idx;
}

/*
 * vesc.bms_val(key [, index]) -> number, boolean or string
 *
 * nil for an unknown key rather than an error, so a script can probe for a
 * field a given firmware may not have.
 */
static int l_bms_val(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	const bms_val_t *v = bms_val_find(key);

	if (!v) {
		lua_pushnil(L);
		return 1;
	}

	switch (v->type) {
	case BV_FLOAT:
		lua_pushnumber(L, *((volatile float *)field_ptr(v)));
		break;

	case BV_INT:
		lua_pushinteger(L, *((volatile int *)field_ptr(v)));
		break;

	case BV_STATUS: {
		// Copied out because the field is volatile and not guaranteed
		// terminated by whatever filled it.
		char buf[BMS_STATUS_LEN + 1];
		volatile char *src = (volatile char *)field_ptr(v);
		size_t i = 0;
		while (i < BMS_STATUS_LEN && src[i] != '\0') {
			buf[i] = src[i];
			i++;
		}
		buf[i] = '\0';
		lua_pushstring(L, buf);
		break;
	}

	case BV_AGE:
		lua_pushnumber(L, UTILS_AGE_S(*((volatile uint32_t *)field_ptr(v))));
		break;

	case BV_CELL_V:
	case BV_TEMP_ADC: {
		int idx = array_index(L, v, 2);
		lua_pushnumber(L, ((volatile float *)field_ptr(v))[idx]);
		break;
	}

	case BV_BAL: {
		int idx = array_index(L, v, 2);
		lua_pushboolean(L, ((volatile bool *)field_ptr(v))[idx]);
		break;
	}

	default:
		lua_pushnil(L);
		break;
	}

	return 1;
}

/*
 * vesc.bms_set_val(key, value [, index])
 *
 * Writing these is how a script acts as the BMS -- the values are then sent
 * on with vesc.bms_send_can(). msg_age is computed and refuses a write.
 */
static int l_bms_set_val(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	const bms_val_t *v = bms_val_find(key);

	if (!v) {
		return luaL_error(L, "bms_set_val: no such value '%s'", key);
	}

	switch (v->type) {
	case BV_FLOAT:
		*((volatile float *)field_ptr(v)) = (float)luaL_checknumber(L, 2);
		break;

	case BV_INT: {
		int n = (int)luaL_checkinteger(L, 2);
		// cell_num and temp_adc_num bound every indexed read, so a value past
		// the array would hand out memory after it.
		if (v->offset == offsetof(bms_values, cell_num) &&
				(n < 0 || n > BMS_MAX_CELLS)) {
			return luaL_error(L, "bms_set_val: cell_num %d is outside 0..%d",
					n, BMS_MAX_CELLS);
		}
		if (v->offset == offsetof(bms_values, temp_adc_num) &&
				(n < 0 || n > BMS_MAX_TEMPS)) {
			return luaL_error(L, "bms_set_val: temp_adc_num %d is outside 0..%d",
					n, BMS_MAX_TEMPS);
		}
		*((volatile int *)field_ptr(v)) = n;
		break;
	}

	case BV_STATUS: {
		size_t len = 0;
		const char *src = luaL_checklstring(L, 2, &len);
		volatile char *dst = (volatile char *)field_ptr(v);
		if (len > BMS_STATUS_LEN - 1) {
			len = BMS_STATUS_LEN - 1;
		}
		for (size_t i = 0;i < len;i++) {
			dst[i] = src[i];
		}
		dst[len] = '\0';
		break;
	}

	case BV_CELL_V:
	case BV_TEMP_ADC: {
		float f = (float)luaL_checknumber(L, 2);
		int idx = array_index(L, v, 3);
		((volatile float *)field_ptr(v))[idx] = f;
		break;
	}

	case BV_BAL: {
		bool b = lua_toboolean(L, 2);
		int idx = array_index(L, v, 3);
		((volatile bool *)field_ptr(v))[idx] = b;
		break;
	}

	case BV_AGE:
	default:
		return luaL_error(L, "bms_set_val: '%s' is read only", key);
	}

	lua_pushboolean(L, 1);
	return 1;
}

/*
 * vesc.bms_cells() -> { v, ... },  vesc.bms_temps(),  vesc.bms_bal()
 *
 * The whole array in one call, which is what a dash actually wants and what
 * the per-index form makes tedious. Length comes from the reported count, so
 * an empty table means no pack has been seen rather than a pack of zero
 * volts.
 */
static int push_array(lua_State *L, const char *key) {
	const bms_val_t *v = bms_val_find(key);
	volatile bms_values *val = bms_get_values();
	int cnt = (v->type == BV_TEMP_ADC) ? val->temp_adc_num : val->cell_num;

	if (cnt < 0) {
		cnt = 0;
	}

	lua_createtable(L, cnt, 0);
	for (int i = 0;i < cnt;i++) {
		if (v->type == BV_BAL) {
			lua_pushboolean(L, ((volatile bool *)field_ptr(v))[i]);
		} else {
			lua_pushnumber(L, ((volatile float *)field_ptr(v))[i]);
		}
		lua_rawseti(L, -2, i + 1);
	}

	return 1;
}

static int l_bms_cells(lua_State *L) {
	return push_array(L, "v_cell");
}

static int l_bms_temps(lua_State *L) {
	return push_array(L, "temps_adc");
}

static int l_bms_bal(lua_State *L) {
	return push_array(L, "bal_state");
}

// vesc.bms_send_can() -- publish the current values as BMS status frames.
static int l_bms_send_can(lua_State *L) {
	(void)L;
	bms_send_status_can();
	return 0;
}

// These three go through bms_process_cmd, the same path a host command takes,
// so a BMS behind the firmware sees them identically.
static int bms_cmd(lua_State *L, uint8_t cmd, bool has_arg) {
	uint8_t data[2];
	unsigned int len = 1;

	data[0] = cmd;
	if (has_arg) {
		data[1] = lua_toboolean(L, 1) ? 1 : 0;
		len = 2;
	}

	bms_process_cmd(data, len, 0);
	lua_pushboolean(L, 1);
	return 1;
}

static int l_bms_chg_allowed(lua_State *L) {
	return bms_cmd(L, COMM_BMS_SET_CHARGE_ALLOWED, true);
}

static int l_bms_force_balance(lua_State *L) {
	return bms_cmd(L, COMM_BMS_FORCE_BALANCE, true);
}

static int l_bms_zero_offset(lua_State *L) {
	return bms_cmd(L, COMM_BMS_ZERO_CURRENT_OFFSET, false);
}

static const luaL_Reg bms_funcs[] = {
	{"bms_val", l_bms_val},
	{"bms_set_val", l_bms_set_val},
	{"bms_cells", l_bms_cells},
	{"bms_temps", l_bms_temps},
	{"bms_bal", l_bms_bal},
	{"bms_send_can", l_bms_send_can},
	{"bms_chg_allowed", l_bms_chg_allowed},
	{"bms_force_balance", l_bms_force_balance},
	{"bms_zero_offset", l_bms_zero_offset},
	{NULL, NULL},
};

void lua_vesc_bms_register(script_lua_t *s) {
	script_lua_register(s, bms_funcs);
}
