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
 * This is a small self-contained I2C touch path rather than a share of the
 * lisp one, and that is a deliberate trade rather than laziness.
 *
 * lispif_touch_extensions.c is 1155 lines in which the driver core and the
 * LispBM bindings are interleaved: validators call lbm_set_error_reason, and
 * touch events are flattened into lisp values and posted to the interpreter's
 * event queue. Guarding the bindings the way the panel drivers were guarded
 * leaves thirty LBM references inside the core, so making that file serve
 * both engines means extracting a neutral core -- a real refactor of code
 * that currently works and that the existing lisp dash depends on.
 *
 * Duplicating a hundred lines of esp_lcd_touch setup costs less than risking
 * that, and it keeps this engine's touch independent while the question is
 * still "does Lua touch work at all". The right end state is one neutral
 * core in main/touch with both engines as thin wrappers; this is not it, and
 * says so.
 *
 * What it does not do, which the lisp side does: interrupt-driven events,
 * multi-touch, and the other four controllers. Polling one point covers a
 * dash, and a script that polls at its own rate is easier to reason about
 * than one woken by an ISR.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "driver/i2c.h"

#include <string.h>

// The same port the lisp touch code and the board's own drivers use. A board
// has one touch bus; a second port number would be a conflict, not a choice.
#define TOUCH_PORT		0

static esp_lcd_panel_io_handle_t m_io = NULL;
static esp_lcd_touch_handle_t m_touch = NULL;
static bool m_owns_bus = false;

static void touch_unload(void) {
	if (m_touch) {
		esp_lcd_touch_del(m_touch);
		m_touch = NULL;
	}
	if (m_io) {
		esp_lcd_panel_io_del(m_io);
		m_io = NULL;
	}
	if (m_owns_bus) {
		i2c_driver_delete(TOUCH_PORT);
		m_owns_bus = false;
	}
}

/*
 * vesc.touch_load_gt911(sda, scl, rst, int_pin, width, height, [freq])
 *
 * int_pin may be -1, and on several boards it has to be: the GT911 samples
 * that pin as its reset is released to choose between I2C address 0x5D and
 * 0x14, so a board that pulls it up answers at the other address from the one
 * the driver expects. Passing -1 leaves the pin alone and polls instead.
 */
static int l_touch_load_gt911(lua_State *L) {
	int sda = (int)luaL_checkinteger(L, 1);
	int scl = (int)luaL_checkinteger(L, 2);
	int rst = (int)luaL_checkinteger(L, 3);
	int int_pin = (int)luaL_checkinteger(L, 4);
	int width = (int)luaL_checkinteger(L, 5);
	int height = (int)luaL_checkinteger(L, 6);
	uint32_t freq = (uint32_t)luaL_optinteger(L, 7, 400000);

	if (width < 1 || height < 1) {
		return luaL_error(L, "touch_load_gt911: %dx%d is not a usable size",
				width, height);
	}
	if (freq < 10000 || freq > 1000000) {
		return luaL_error(L, "touch_load_gt911: %u Hz is outside 10k..1M",
				(unsigned)freq);
	}

	touch_unload();

	/*
	 * The bus may already be up: a board's own hw_init can have claimed it
	 * for a display helper or an expander on the same pins. Installing it
	 * twice returns an error and, worse, deleting it on unload would pull the
	 * bus out from under whatever else is using it -- so ownership is
	 * recorded and only a bus opened here is ever closed here.
	 */
	const i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = sda,
		.scl_io_num = scl,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = freq,
	};

	if (i2c_param_config(TOUCH_PORT, &conf) != ESP_OK) {
		return luaL_error(L, "touch_load_gt911: bad I2C pins");
	}

	esp_err_t bus = i2c_driver_install(TOUCH_PORT, conf.mode, 0, 0, 0);
	if (bus == ESP_OK) {
		m_owns_bus = true;
	} else if (bus != ESP_ERR_INVALID_STATE) {
		// INVALID_STATE means somebody else already installed it, which is
		// fine. Anything else is a real failure.
		return luaL_error(L, "touch_load_gt911: I2C bus failed (%s)",
				esp_err_to_name(bus));
	}

	esp_lcd_panel_io_i2c_config_t io_conf = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
	io_conf.scl_speed_hz = 0;	// take the bus speed configured above

	esp_err_t res = esp_lcd_new_panel_io_i2c(TOUCH_PORT, &io_conf, &m_io);
	if (res != ESP_OK) {
		touch_unload();
		return luaL_error(L, "touch_load_gt911: panel io failed (%s)",
				esp_err_to_name(res));
	}

	esp_lcd_touch_io_gt911_config_t gt911_cfg = {
		.dev_addr = io_conf.dev_addr,
	};

	esp_lcd_touch_config_t tp_cfg = {
		.x_max = (uint16_t)width,
		.y_max = (uint16_t)height,
		.rst_gpio_num = rst >= 0 ? (gpio_num_t)rst : GPIO_NUM_NC,
		.int_gpio_num = int_pin >= 0 ? (gpio_num_t)int_pin : GPIO_NUM_NC,
		.levels = { .reset = 0, .interrupt = 0 },
		.flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
		.driver_data = &gt911_cfg,
	};

	res = esp_lcd_touch_new_i2c_gt911(m_io, &tp_cfg, &m_touch);
	if (res != ESP_OK) {
		touch_unload();
		return luaL_error(L, "touch_load_gt911: controller did not answer "
				"(%s). On some boards the INT pin selects the address -- try "
				"-1 for it.", esp_err_to_name(res));
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int l_touch_unload(lua_State *L) {
	touch_unload();
	lua_pushboolean(L, 1);
	return 1;
}

/*
 * vesc.touch_read() -> x, y   or   nil when nothing is touching.
 *
 * Returning nil rather than the last coordinates is the whole point: a dash
 * needs to know a finger has lifted, and a stale coordinate pair looks exactly
 * like a finger that has stopped moving.
 */
static int l_touch_read(lua_State *L) {
	if (!m_touch) {
		return luaL_error(L, "touch_read: call touch_load_gt911 first");
	}

	esp_err_t res = esp_lcd_touch_read_data(m_touch);
	if (res != ESP_OK) {
		lua_pushnil(L);
		lua_pushstring(L, esp_err_to_name(res));
		return 2;
	}

	uint16_t x[1] = {0};
	uint16_t y[1] = {0};
	uint16_t strength[1] = {0};
	uint8_t count = 0;

	if (!esp_lcd_touch_get_coordinates(m_touch, x, y, strength, &count, 1) ||
			count == 0) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, (lua_Integer)x[0]);
	lua_pushinteger(L, (lua_Integer)y[0]);
	lua_pushinteger(L, (lua_Integer)strength[0]);
	return 3;
}

/*
 * vesc.touch_transform(swap_xy, mirror_x, mirror_y)
 *
 * A rotated display needs this: the panel here is 480x800 native and driven
 * as 800x480, while the controller keeps reporting in its own frame, so touch
 * lands at the wrong place until the axes are brought into agreement.
 *
 * The mirror swap below is not redundant. When a driver has no native
 * set_swap_xy, esp_lcd_touch applies the generic swap after mirroring, so the
 * mirror flags refer to the pre-swap axes and have to be exchanged to mean
 * what the caller intended. The lisp engine does the same thing in
 * ext_touch_apply_transforms; getting it wrong gives a mapping that is right
 * for rotation but mirrored on one axis, which is easy to mistake for a
 * miscalibrated panel.
 */
static int l_touch_transform(lua_State *L) {
	if (!m_touch) {
		return luaL_error(L, "touch_transform: call touch_load_gt911 first");
	}

	bool swap_xy = lua_toboolean(L, 1);
	bool mirror_x = lua_toboolean(L, 2);
	bool mirror_y = lua_toboolean(L, 3);

	bool apply_x = mirror_x;
	bool apply_y = mirror_y;
	if (swap_xy && m_touch->set_swap_xy == NULL) {
		apply_x = mirror_y;
		apply_y = mirror_x;
	}

	esp_err_t res = esp_lcd_touch_set_swap_xy(m_touch, swap_xy);
	if (res == ESP_OK) {
		res = esp_lcd_touch_set_mirror_x(m_touch, apply_x);
	}
	if (res == ESP_OK) {
		res = esp_lcd_touch_set_mirror_y(m_touch, apply_y);
	}

	if (res != ESP_OK) {
		return luaL_error(L, "touch_transform failed (%s)",
				esp_err_to_name(res));
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int l_touch_loaded(lua_State *L) {
	lua_pushboolean(L, m_touch != NULL);
	return 1;
}

static const luaL_Reg touch_fns[] = {
	{"touch_load_gt911", l_touch_load_gt911},
	{"touch_unload", l_touch_unload},
	{"touch_read", l_touch_read},
	{"touch_transform", l_touch_transform},
	{"touch_loaded", l_touch_loaded},
	{NULL, NULL},
};

void lua_vesc_touch_register(script_lua_t *s) {
	script_lua_register(s, touch_fns);
}
