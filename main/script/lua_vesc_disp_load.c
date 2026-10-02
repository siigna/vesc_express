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
 * Panel loaders for the Lua engine.
 *
 * Separate from lua_vesc_disp.c for one reason: this file names real panel
 * drivers, which only build against ESP-IDF, while the drawing bindings next
 * door are pure arithmetic over tinygfx and run in the host tests. Keeping
 * the hardware-only part in its own file is what lets the drawing stay
 * testable without a board -- see main/script/test.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "disp_backend.h"
#include "tinygfx.h"

#include "disp_st7789.h"
#include "disp_st7789a.h"
#include "disp_ili9341.h"
#include "disp_ili9488.h"
#include "disp_gc9a01.h"
#include "disp_jd9853.h"
#include "disp_sh8601.h"
#include "disp_ssd1351.h"
#include "disp_st7735.h"
#include "disp_sh8501b.h"
#include "disp_icna3306.h"
#include "disp_ssd1306.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "disp_st7701.h"
#endif

#include <string.h>

// ---------------------------------------------------------------- loaders ---

/*
 * Panel loaders.
 *
 * The lisp engine has one extension per panel (disp-load-st7789 and friends),
 * all living in the interpreter's extension file, which is why a Lua build
 * could draw but not bring a panel up. Rather than transcribe seventeen
 * near-identical wrappers, this is one binding with a table: almost every SPI
 * panel in the tree takes the same six arguments, and the few that differ are
 * spelled out.
 *
 *   vesc.disp_load("st7789", sd0, clk, cs, reset, dc, mhz)
 *   vesc.disp_load("ssd1306", sda, scl, hz)
 *
 * Loading registers the driver's callbacks with disp_backend, which is what
 * makes the drawing functions above start reaching the glass.
 */

typedef enum {
	LOADER_SPI6,	// sd0, clk, cs, reset, dc, mhz
	LOADER_SPI5,	// sd0, clk, cs, reset, mhz -- no data/command pin
	LOADER_I2C,	// sda, scl, hz
	LOADER_DSI,	// rst, lane_mbps -- MIPI-DSI, so no pin list to pass
} loader_kind_t;

typedef struct {
	const char *name;
	loader_kind_t kind;
	void (*init6)(int, int, int, int, int, int);
	void (*init5)(int, int, int, int, int);
	void (*init_i2c)(int, int, uint32_t);
	bool (*init_dsi)(int, int);
	disp_orientation_fn orientation;
	disp_render_fn render;
	disp_clear_fn clear;
	disp_reset_fn reset;
} loader_t;

/*
 * Designated initialisers, not positional. The struct has ten fields, four of
 * which are function pointers that differ only in arity, and a positional
 * list put two panels' init functions in the DSI slot -- which the compiler
 * caught only because the arities happened to differ. Naming the fields makes
 * that class of mistake impossible rather than merely detectable.
 */
static const loader_t m_loaders[] = {
	{.name = "st7789", .kind = LOADER_SPI6, .init6 = disp_st7789_init,
		.render = disp_st7789_render_image, .clear = disp_st7789_clear,
		.reset = disp_st7789_reset},
	{.name = "st7789a", .kind = LOADER_SPI6, .init6 = disp_st7789a_init,
		.render = disp_st7789a_render_image, .clear = disp_st7789a_clear,
		.reset = disp_st7789a_reset},
	{.name = "ili9341", .kind = LOADER_SPI6, .init6 = disp_ili9341_init,
		.render = disp_ili9341_render_image, .clear = disp_ili9341_clear,
		.reset = disp_ili9341_reset},
	{.name = "ili9488", .kind = LOADER_SPI6, .init6 = disp_ili9488_init,
		.render = disp_ili9488_render_image, .clear = disp_ili9488_clear,
		.reset = disp_ili9488_reset},
	{.name = "gc9a01", .kind = LOADER_SPI6, .init6 = disp_gc9a01_init,
		.render = disp_gc9a01_render_image, .clear = disp_gc9a01_clear,
		.reset = disp_gc9a01_reset},
	{.name = "jd9853", .kind = LOADER_SPI6, .init6 = disp_jd9853_init,
		.render = disp_jd9853_render_image, .clear = disp_jd9853_clear,
		.reset = disp_jd9853_reset},
	{.name = "sh8601", .kind = LOADER_SPI6, .init6 = disp_sh8601_init,
		.render = disp_sh8601_render_image, .clear = disp_sh8601_clear,
		.reset = disp_sh8601_reset},
	{.name = "ssd1351", .kind = LOADER_SPI6, .init6 = disp_ssd1351_init,
		.render = disp_ssd1351_render_image, .clear = disp_ssd1351_clear,
		.reset = disp_ssd1351_reset},
	{.name = "st7735", .kind = LOADER_SPI6, .init6 = disp_st7735_init,
		.render = disp_st7735_render_image, .clear = disp_st7735_clear,
		.reset = disp_st7735_reset},
	{.name = "sh8501b", .kind = LOADER_SPI5, .init5 = disp_sh8501b_init,
		.render = disp_sh8501b_render_image, .clear = disp_sh8501b_clear,
		.reset = disp_sh8501b_reset},
	{.name = "icna3306", .kind = LOADER_SPI5, .init5 = disp_icna3306_init,
		.render = disp_icna3306_render_image, .clear = disp_icna3306_clear,
		.reset = disp_icna3306_reset},
	{.name = "ssd1306", .kind = LOADER_I2C, .init_i2c = disp_ssd1306_init,
		.render = disp_ssd1306_render_image, .clear = disp_ssd1306_clear,
		.reset = disp_ssd1306_reset},
#if CONFIG_IDF_TARGET_ESP32P4
	/*
	 * MIPI-DSI rather than a parallel or SPI bus, so the only arguments are
	 * the reset pin and the lane rate -- the pixel path belongs to the DSI
	 * peripheral. This panel also rotates, via a software transpose in the
	 * driver, so it registers an orientation handler.
	 */
	{.name = "st7701", .kind = LOADER_DSI, .init_dsi = disp_st7701_init,
		.orientation = disp_st7701_set_orientation,
		.render = disp_st7701_render_image, .clear = disp_st7701_clear,
		.reset = disp_st7701_reset},
#endif
};

static int l_disp_load(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);

	const loader_t *ld = NULL;
	for (size_t i = 0; i < sizeof(m_loaders) / sizeof(m_loaders[0]); i++) {
		if (strcmp(m_loaders[i].name, name) == 0) {
			ld = &m_loaders[i];
			break;
		}
	}

	if (!ld) {
		/*
		 * List what is available in the error. A wrong panel name is the most
		 * likely mistake when bringing a board up, and the alternative is a
		 * script author guessing at spellings.
		 */
		luaL_Buffer b;
		luaL_buffinit(L, &b);
		luaL_addstring(&b, "disp_load: unknown panel '");
		luaL_addstring(&b, name);
		luaL_addstring(&b, "'. Available: ");
		for (size_t i = 0; i < sizeof(m_loaders) / sizeof(m_loaders[0]); i++) {
			if (i > 0) {
				luaL_addstring(&b, ", ");
			}
			luaL_addstring(&b, m_loaders[i].name);
		}
		luaL_pushresult(&b);
		return lua_error(L);
	}

	switch (ld->kind) {
	case LOADER_SPI6:
		ld->init6((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
				(int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5),
				(int)luaL_checkinteger(L, 6),
				(int)luaL_optinteger(L, 7, 40));
		break;

	case LOADER_SPI5:
		ld->init5((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
				(int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5),
				(int)luaL_optinteger(L, 6, 40));
		break;

	case LOADER_I2C:
		ld->init_i2c((int)luaL_checkinteger(L, 2),
				(int)luaL_checkinteger(L, 3),
				(uint32_t)luaL_optinteger(L, 4, 700000));
		break;

	case LOADER_DSI: {
		int rst = (int)luaL_checkinteger(L, 2);
		int mbps = (int)luaL_optinteger(L, 3, 500);
		if (mbps <= 0 || mbps > 4000) {
			return luaL_error(L, "disp_load: %d Mbps is not a usable DSI lane "
					"rate", mbps);
		}
		// Unlike the others this one reports failure, since bringing up a DSI
		// link can fail for reasons a pin list cannot: the PHY LDO, the lane
		// rate, the panel not answering.
		if (!ld->init_dsi(rst, mbps)) {
			return luaL_error(L, "disp_load: the ST7701 did not come up. "
					"Check the reset pin and the lane rate.");
		}
	} break;
	}

	disp_backend_set(ld->render, ld->clear, ld->reset);
	if (ld->orientation) {
		disp_backend_set_orientation_fn(ld->orientation);
	}

	lua_pushboolean(L, 1);
	return 1;
}

// The panels this build can load, so a script can adapt rather than guess.
static int l_disp_panels(lua_State *L) {
	lua_newtable(L);
	for (size_t i = 0; i < sizeof(m_loaders) / sizeof(m_loaders[0]); i++) {
		lua_pushstring(L, m_loaders[i].name);
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}
	return 1;
}


/*
 * vesc.disp_orientation(0..3)
 *
 * Only some panels can rotate. Saying so beats accepting the call and
 * doing nothing, which is how a dash ends up sideways with no error.
 */
static int l_disp_orientation(lua_State *L) {
	int rot = (int)luaL_checkinteger(L, 1);

	disp_orientation_fn fn = disp_backend_orientation();
	if (!fn) {
		return luaL_error(L, "disp_orientation: the loaded panel cannot "
				"rotate (or none is loaded)");
	}
	if (!fn(rot)) {
		return luaL_error(L, "disp_orientation: %d is not 0, 1, 2 or 3", rot);
	}

	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg loader_fns[] = {
	{"disp_load", l_disp_load},
	{"disp_orientation", l_disp_orientation},
	{"disp_panels", l_disp_panels},
	{NULL, NULL},
};

void lua_vesc_disp_load_register(script_lua_t *s) {
	script_lua_register(s, loader_fns);
}
