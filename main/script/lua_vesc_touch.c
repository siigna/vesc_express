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
 * Touch bindings for the Lua engine.
 *
 * These are a thin layer over touch_core, which is the same hardware path the
 * LispBM bindings use. This file previously carried its own self-contained
 * I2C setup for the GT911 alone -- a stopgap, and it said so -- which cost
 * this engine the other five controllers and multi-touch.
 *
 * Still not here: interrupt-driven touch events. The core can deliver them,
 * but the Lua side would need a new entry in script_event_t and a handler
 * name in the dispatch table, and polling one point covers a dash. A script
 * that polls at its own rate is also easier to reason about than one woken by
 * an ISR.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "touch_core.h"
#include "esp_lcd_touch.h"

#include <string.h>

#define TOUCH_FREQ_MIN		10000
#define TOUCH_FREQ_MAX		1000000

static int touch_err(lua_State *L, const char *what, esp_err_t res) {
	return luaL_error(L, "%s: %s", what, esp_err_to_name(res));
}

/*
 * vesc.touch_load_<part>(sda, scl, rst, int_pin, width, height, [freq])
 *
 * int_pin may be -1, and on several boards it has to be: the GT911 samples
 * that pin as its reset is released to choose between I2C address 0x5D and
 * 0x14, so a board that pulls it up answers at the other address from the one
 * the driver expects. Passing -1 leaves the pin alone and polls instead --
 * though the core probes both addresses either way.
 */
static int touch_load_i2c(lua_State *L, touch_part_t part, const char *what) {
	int sda = (int)luaL_checkinteger(L, 1);
	int scl = (int)luaL_checkinteger(L, 2);
	int rst = (int)luaL_checkinteger(L, 3);
	int int_pin = (int)luaL_checkinteger(L, 4);
	int width = (int)luaL_checkinteger(L, 5);
	int height = (int)luaL_checkinteger(L, 6);
	uint32_t freq = (uint32_t)luaL_optinteger(L, 7, 400000);

	if (width < 1 || height < 1) {
		return luaL_error(L, "%s: %dx%d is not a usable size", what, width, height);
	}
	if (freq < TOUCH_FREQ_MIN || freq > TOUCH_FREQ_MAX) {
		return luaL_error(L, "%s: %u Hz is outside 10k..1M", what, (unsigned)freq);
	}
	if (!touch_core_gpio_valid_or_nc(sda) || !touch_core_gpio_valid_or_nc(scl) ||
			!touch_core_gpio_valid_or_nc(rst) || !touch_core_gpio_valid_or_nc(int_pin)) {
		return luaL_error(L, "%s: bad pin number", what);
	}

	if (!touch_core_init()) {
		return luaL_error(L, "%s: touch runtime init failed", what);
	}

	esp_err_t res = touch_core_load_i2c(part, sda, scl, rst, int_pin,
			(uint16_t)width, (uint16_t)height, freq);
	if (res != ESP_OK) {
		return touch_err(L, what, res);
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int l_touch_load_gt911(lua_State *L) {
	return touch_load_i2c(L, TOUCH_PART_GT911, "touch_load_gt911");
}

static int l_touch_load_cst816s(lua_State *L) {
	return touch_load_i2c(L, TOUCH_PART_CST816S, "touch_load_cst816s");
}

static int l_touch_load_cst9217(lua_State *L) {
	return touch_load_i2c(L, TOUCH_PART_CST9217, "touch_load_cst9217");
}

static int l_touch_load_axs15231(lua_State *L) {
	return touch_load_i2c(L, TOUCH_PART_AXS15231, "touch_load_axs15231");
}

static int l_touch_load_cst836u(lua_State *L) {
	return touch_load_i2c(L, TOUCH_PART_CST836U, "touch_load_cst836u");
}

// vesc.touch_load_xpt2046(host, mosi, miso, sclk, cs, int_pin, w, h, [freq])
static int l_touch_load_xpt2046(lua_State *L) {
	int host = (int)luaL_checkinteger(L, 1);
	int mosi = (int)luaL_checkinteger(L, 2);
	int miso = (int)luaL_checkinteger(L, 3);
	int sclk = (int)luaL_checkinteger(L, 4);
	int cs = (int)luaL_checkinteger(L, 5);
	int int_pin = (int)luaL_checkinteger(L, 6);
	int width = (int)luaL_checkinteger(L, 7);
	int height = (int)luaL_checkinteger(L, 8);
	uint32_t freq = (uint32_t)luaL_optinteger(L, 9, 2500000);

	if (width < 1 || height < 1) {
		return luaL_error(L, "touch_load_xpt2046: %dx%d is not a usable size",
				width, height);
	}

	if (!touch_core_init()) {
		return luaL_error(L, "touch_load_xpt2046: touch runtime init failed");
	}

	esp_err_t res = touch_core_load_spi(TOUCH_PART_XPT2046, host, mosi, miso,
			sclk, cs, int_pin, (uint16_t)width, (uint16_t)height, freq);
	if (res != ESP_OK) {
		return touch_err(L, "touch_load_xpt2046", res);
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int l_touch_unload(lua_State *L) {
	(void)L;
	touch_core_delete();
	return 0;
}

/*
 * vesc.touch_read() -> x, y, strength, track_id  or  nil when untouched.
 *
 * Returns nil rather than raising when nothing is loaded either, so a script
 * can poll unconditionally on a board whose touch did not come up.
 */
static int l_touch_read(lua_State *L) {
	touch_point_t point;
	uint8_t cnt = 0;

	if (touch_core_read(&point, &cnt, 1) != ESP_OK || cnt == 0) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, point.x);
	lua_pushinteger(L, point.y);
	lua_pushinteger(L, point.strength);
	lua_pushinteger(L, point.track_id);
	return 4;
}

/*
 * vesc.touch_read_all() -> { {x=,y=,strength=,track_id=}, ... }
 *
 * An empty table when untouched, so the caller can take # of it without a nil
 * check. Multi-touch is what the core reports; how many points a part gives
 * is the part's business.
 */
static int l_touch_read_all(lua_State *L) {
	touch_point_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
	uint8_t cnt = 0;

	if (touch_core_read(points, &cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS) != ESP_OK) {
		cnt = 0;
	}

	lua_createtable(L, cnt, 0);
	for (uint8_t i = 0;i < cnt;i++) {
		lua_createtable(L, 0, 4);
		lua_pushinteger(L, points[i].x);
		lua_setfield(L, -2, "x");
		lua_pushinteger(L, points[i].y);
		lua_setfield(L, -2, "y");
		lua_pushinteger(L, points[i].strength);
		lua_setfield(L, -2, "strength");
		lua_pushinteger(L, points[i].track_id);
		lua_setfield(L, -2, "track_id");
		lua_rawseti(L, -2, i + 1);
	}

	return 1;
}

// vesc.touch_transform(swap_xy, mirror_x, mirror_y)
static int l_touch_transform(lua_State *L) {
	bool swap_xy = lua_toboolean(L, 1);
	bool mirror_x = lua_toboolean(L, 2);
	bool mirror_y = lua_toboolean(L, 3);

	esp_err_t res = touch_core_set_transforms(swap_xy, mirror_x, mirror_y);
	if (res == ESP_ERR_INVALID_STATE) {
		return luaL_error(L, "touch_transform: touch not loaded");
	}
	if (res != ESP_OK) {
		return touch_err(L, "touch_transform", res);
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int l_touch_loaded(lua_State *L) {
	lua_pushboolean(L, touch_core_loaded());
	return 1;
}

static const luaL_Reg touch_funcs[] = {
	{"touch_load_gt911", l_touch_load_gt911},
	{"touch_load_cst816s", l_touch_load_cst816s},
	{"touch_load_cst9217", l_touch_load_cst9217},
	{"touch_load_axs15231", l_touch_load_axs15231},
	{"touch_load_cst836u", l_touch_load_cst836u},
	{"touch_load_xpt2046", l_touch_load_xpt2046},
	{"touch_unload", l_touch_unload},
	{"touch_read", l_touch_read},
	{"touch_read_all", l_touch_read_all},
	{"touch_transform", l_touch_transform},
	{"touch_loaded", l_touch_loaded},
	{NULL, NULL},
};

void lua_vesc_touch_register(script_lua_t *s) {
	script_lua_register(s, touch_funcs);
}
