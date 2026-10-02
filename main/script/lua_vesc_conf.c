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
 * Config bindings for the Lua engine: the board's own settings, as VESC Tool
 * writes them -- controller id, CAN rate, wifi and BLE.
 *
 * A descriptor table of offsetof rows, as in lua_vesc_bms.c, rather than the
 * symbol if-chain the lisp side dispatches through. Keys take either
 * separator, so one copied out of a lisp script works.
 *
 * Writes land in the live config and take effect where the firmware reads it
 * again; vesc.conf_store() persists them. Changing the CAN baud rate applies
 * it to the running interface, as the lisp binding does, because the config
 * value alone would leave the hardware on the old rate.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "main.h"
#include "datatypes.h"
#include "comm_can.h"

#include <stddef.h>
#include <string.h>

typedef enum {
	CV_INT,
	CV_BOOL,
	CV_U16,
	CV_U32,
	CV_STRING,
	CV_CAN_BAUD,	// Needs the running interface updated on a write
} conf_val_type_t;

typedef struct {
	const char *name;
	conf_val_type_t type;
	size_t offset;
	size_t size;	// Strings only: the field's capacity including the NUL
} conf_val_t;

static const conf_val_t conf_vals[] = {
	{"controller_id",			CV_INT,			offsetof(main_config_t, controller_id), 0},
	{"can_baud_rate",			CV_CAN_BAUD,	offsetof(main_config_t, can_baud_rate), 0},
	{"can_status_rate_hz",		CV_INT,			offsetof(main_config_t, can_status_rate_hz), 0},
	{"wifi_mode",				CV_INT,			offsetof(main_config_t, wifi_mode), 0},
	{"wifi_sta_ssid",			CV_STRING,		offsetof(main_config_t, wifi_sta_ssid), sizeof(((main_config_t *)0)->wifi_sta_ssid)},
	{"wifi_sta_key",			CV_STRING,		offsetof(main_config_t, wifi_sta_key), sizeof(((main_config_t *)0)->wifi_sta_key)},
	{"wifi_ap_ssid",			CV_STRING,		offsetof(main_config_t, wifi_ap_ssid), sizeof(((main_config_t *)0)->wifi_ap_ssid)},
	{"wifi_ap_key",				CV_STRING,		offsetof(main_config_t, wifi_ap_key), sizeof(((main_config_t *)0)->wifi_ap_key)},
	{"use_tcp_local",			CV_BOOL,		offsetof(main_config_t, use_tcp_local), 0},
	{"use_tcp_hub",				CV_BOOL,		offsetof(main_config_t, use_tcp_hub), 0},
	{"tcp_hub_url",				CV_STRING,		offsetof(main_config_t, tcp_hub_url), sizeof(((main_config_t *)0)->tcp_hub_url)},
	{"tcp_hub_port",			CV_U16,			offsetof(main_config_t, tcp_hub_port), 0},
	{"tcp_hub_id",				CV_STRING,		offsetof(main_config_t, tcp_hub_id), sizeof(((main_config_t *)0)->tcp_hub_id)},
	{"tcp_hub_pass",			CV_STRING,		offsetof(main_config_t, tcp_hub_pass), sizeof(((main_config_t *)0)->tcp_hub_pass)},
	{"ble_mode",				CV_INT,			offsetof(main_config_t, ble_mode), 0},
	{"ble_name",				CV_STRING,		offsetof(main_config_t, ble_name), sizeof(((main_config_t *)0)->ble_name)},
	{"ble_pin",					CV_U32,			offsetof(main_config_t, ble_pin), 0},
	{"ble_service_capacity",	CV_U32,			offsetof(main_config_t, ble_service_capacity), 0},
	{"ble_chr_descr_capacity",	CV_U32,			offsetof(main_config_t, ble_chr_descr_capacity), 0},
};

#define CONF_VAL_CNT	(sizeof(conf_vals) / sizeof(conf_vals[0]))

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

static const conf_val_t *conf_find(const char *key) {
	for (size_t i = 0;i < CONF_VAL_CNT;i++) {
		if (key_eq(key, conf_vals[i].name)) {
			return &conf_vals[i];
		}
	}

	return NULL;
}

static void *conf_ptr(const conf_val_t *v) {
	return (void *)((char *)&backup.config + v->offset);
}

// vesc.conf_get(key) -> number, boolean or string; nil for an unknown key.
static int l_conf_get(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	const conf_val_t *v = conf_find(key);

	if (!v) {
		lua_pushnil(L);
		return 1;
	}

	switch (v->type) {
	case CV_INT:
	case CV_CAN_BAUD:
		lua_pushinteger(L, *((volatile int *)conf_ptr(v)));
		break;
	case CV_BOOL:
		lua_pushboolean(L, *((volatile bool *)conf_ptr(v)));
		break;
	case CV_U16:
		lua_pushinteger(L, *((volatile uint16_t *)conf_ptr(v)));
		break;
	case CV_U32:
		lua_pushinteger(L, *((volatile uint32_t *)conf_ptr(v)));
		break;
	case CV_STRING: {
		// Copied out: the field is volatile, and a config written by an older
		// firmware is not guaranteed terminated.
		char buf[64];
		volatile char *src = (volatile char *)conf_ptr(v);
		size_t cap = v->size < sizeof(buf) ? v->size : sizeof(buf);
		size_t i = 0;
		while (i + 1 < cap && src[i] != '\0') {
			buf[i] = src[i];
			i++;
		}
		buf[i] = '\0';
		lua_pushstring(L, buf);
		break;
	}
	default:
		lua_pushnil(L);
		break;
	}

	return 1;
}

// vesc.conf_set(key, value) -> true. Call conf_store() to persist.
static int l_conf_set(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	const conf_val_t *v = conf_find(key);

	if (!v) {
		return luaL_error(L, "conf_set: no such setting '%s'", key);
	}

	switch (v->type) {
	case CV_INT:
		*((volatile int *)conf_ptr(v)) = (int)luaL_checkinteger(L, 2);
		break;

	case CV_CAN_BAUD: {
		int rate = (int)luaL_checkinteger(L, 2);
		volatile int *field = (volatile int *)conf_ptr(v);
		if (*field != rate) {
			*field = rate;
			// The config value alone would leave the interface on the old
			// rate, which looks like a dead bus rather than a setting that
			// did not take.
			comm_can_update_baudrate(0);
		}
		break;
	}

	case CV_BOOL:
		*((volatile bool *)conf_ptr(v)) = lua_toboolean(L, 2);
		break;

	case CV_U16:
		*((volatile uint16_t *)conf_ptr(v)) = (uint16_t)luaL_checkinteger(L, 2);
		break;

	case CV_U32:
		*((volatile uint32_t *)conf_ptr(v)) = (uint32_t)luaL_checkinteger(L, 2);
		break;

	case CV_STRING: {
		size_t len = 0;
		const char *src = luaL_checklstring(L, 2, &len);
		if (len >= v->size) {
			return luaL_error(L, "conf_set: '%s' holds %d characters, not %d",
					key, (int)v->size - 1, (int)len);
		}
		volatile char *dst = (volatile char *)conf_ptr(v);
		for (size_t i = 0;i < len;i++) {
			dst[i] = src[i];
		}
		dst[len] = '\0';
		break;
	}

	default:
		return luaL_error(L, "conf_set: '%s' cannot be written", key);
	}

	lua_pushboolean(L, 1);
	return 1;
}

// vesc.conf_store() -- persist the config across reboots.
static int l_conf_store(lua_State *L) {
	(void)L;
	main_store_backup_data();
	return 0;
}

/*
 * vesc.conf_keys() -> { "controller_id", ... }
 *
 * No lisp counterpart. The lisp version needs a symbol the script already
 * knows; a Lua script taking a key from a config screen or a message wants to
 * know what is accepted without guessing at an error.
 */
static int l_conf_keys(lua_State *L) {
	lua_createtable(L, CONF_VAL_CNT, 0);
	for (size_t i = 0;i < CONF_VAL_CNT;i++) {
		lua_pushstring(L, conf_vals[i].name);
		lua_rawseti(L, -2, (int)i + 1);
	}
	return 1;
}

static const luaL_Reg conf_funcs[] = {
	{"conf_get", l_conf_get},
	{"conf_set", l_conf_set},
	{"conf_store", l_conf_store},
	{"conf_keys", l_conf_keys},
	{NULL, NULL},
};

void lua_vesc_conf_register(script_lua_t *s) {
	script_lua_register(s, conf_funcs);
}
