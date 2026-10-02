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
 * Drawing bindings for the Lua engine.
 *
 * The drawing itself is tinygfx, the same code the lisp engine draws with, so
 * both engines put identical pixels in a buffer. Only the binding layer
 * differs, and it differs in one way worth knowing about: image buffers are
 * Lua userdata.
 *
 * LispBM allocates image buffers out of a defrag pool the script creates and
 * sizes by hand (dm-create), and a script that gets that size wrong fails at
 * the point of drawing. Here a buffer is ordinary Lua userdata, so it is
 * counted against the engine's memory ceiling, collected when the last
 * reference goes, and needs no pool to be declared up front. A script that
 * asks for more than the ceiling allows gets a normal Lua out-of-memory
 * error at the point of allocation instead.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "disp_backend.h"
#include "tinygfx.h"


#include <string.h>

#define IMG_MT	"vesc.img"

/*
 * Userdata layout: the header tinygfx wants, then the pixels immediately
 * after it in the same allocation. One allocation per buffer, and the pixels
 * cannot outlive the header.
 */
typedef struct {
	image_buffer_t img;
} img_ud_t;

static img_ud_t *check_img(lua_State *L, int idx) {
	return (img_ud_t *)luaL_checkudata(L, idx, IMG_MT);
}

static color_format_t fmt_from_name(lua_State *L, const char *name) {
	if (strcmp(name, "indexed2") == 0) { return indexed2; }
	if (strcmp(name, "indexed4") == 0) { return indexed4; }
	if (strcmp(name, "indexed16") == 0) { return indexed16; }
	if (strcmp(name, "rgb332") == 0) { return rgb332; }
	if (strcmp(name, "rgb565") == 0) { return rgb565; }
	if (strcmp(name, "rgb888") == 0) { return rgb888; }
	luaL_error(L, "unknown image format '%s' (expected indexed2, indexed4, "
			"indexed16, rgb332, rgb565 or rgb888)", name);
	return format_not_supported;
}

/*
 * vesc.img_buffer(fmt, width, height)
 *
 * The indexed formats are the ones to reach for on a small display: an
 * indexed4 buffer is a quarter the size of rgb565 for the same area, and a
 * dash that draws in a handful of colours does not need more. The palette is
 * supplied at render time, so the same buffer can be drawn in different
 * colours without redrawing it.
 */
static int l_img_buffer(lua_State *L) {
	const char *fmt_name = luaL_checkstring(L, 1);
	lua_Integer w = luaL_checkinteger(L, 2);
	lua_Integer h = luaL_checkinteger(L, 3);

	if (w < 1 || h < 1 || w > 4096 || h > 4096) {
		return luaL_error(L, "img_buffer: %dx%d is not a usable size",
				(int)w, (int)h);
	}

	color_format_t fmt = fmt_from_name(L, fmt_name);
	uint32_t bytes = image_dims_to_size_bytes(fmt, (uint16_t)w, (uint16_t)h);
	if (bytes == 0) {
		return luaL_error(L, "img_buffer: %s at %dx%d has no valid size",
				fmt_name, (int)w, (int)h);
	}

	// Allocated through Lua, so it counts against the memory ceiling and is
	// collected with the buffer. Asking for too much raises out-of-memory
	// here rather than corrupting a drawing call later.
	img_ud_t *ud = (img_ud_t *)lua_newuserdatauv(L,
			sizeof(img_ud_t) + (size_t)bytes, 0);
	memset(ud, 0, sizeof(img_ud_t) + (size_t)bytes);

	uint8_t *pixels = (uint8_t *)(ud + 1);
	ud->img.fmt = fmt;
	ud->img.width = (uint16_t)w;
	ud->img.height = (uint16_t)h;
	ud->img.data = pixels;
	ud->img.mem_base = pixels;

	luaL_setmetatable(L, IMG_MT);
	return 1;
}

static int l_img_dims(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	lua_pushinteger(L, ud->img.width);
	lua_pushinteger(L, ud->img.height);
	return 2;
}

static int l_img_clear(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	uint32_t c = (uint32_t)luaL_optinteger(L, 2, 0);
	image_buffer_clear(&ud->img, c);
	return 0;
}

static int l_img_setpix(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	uint32_t c = (uint32_t)luaL_checkinteger(L, 4);
	// tinygfx clips, so an off-buffer coordinate is a no-op rather than an
	// error. That matches how a dash draws: elements are positioned by
	// arithmetic and running slightly off the edge is normal.
	putpixel(&ud->img, x, y, c);
	return 0;
}

static int l_img_getpix(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	if (x < 0 || y < 0 || x >= ud->img.width || y >= ud->img.height) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, (lua_Integer)getpixel(&ud->img, x, y));
	return 1;
}

static int l_img_line(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x0 = (int)luaL_checkinteger(L, 2);
	int y0 = (int)luaL_checkinteger(L, 3);
	int x1 = (int)luaL_checkinteger(L, 4);
	int y1 = (int)luaL_checkinteger(L, 5);
	uint32_t c = (uint32_t)luaL_checkinteger(L, 6);
	int thickness = (int)luaL_optinteger(L, 7, 1);
	int dot1 = (int)luaL_optinteger(L, 8, 0);
	int dot2 = (int)luaL_optinteger(L, 9, 0);
	tinygfx_line(&ud->img, x0, y0, x1, y1, thickness, dot1, dot2, c);
	return 0;
}

static int l_img_rectangle(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	int w = (int)luaL_checkinteger(L, 4);
	int h = (int)luaL_checkinteger(L, 5);
	uint32_t c = (uint32_t)luaL_checkinteger(L, 6);
	bool filled = lua_toboolean(L, 7);
	int thickness = (int)luaL_optinteger(L, 8, 1);
	int radius = (int)luaL_optinteger(L, 9, 0);

	// dot1/dot2 are tinygfx's dashed-border controls; 0 means solid, which
	// is what a plain rectangle call should give.
	if (radius > 0) {
		if (filled) {
			tinygfx_fill_rounded_rectangle(&ud->img, x, y, w, h, radius, c);
		} else {
			tinygfx_rounded_rectangle(&ud->img, x, y, w, h, radius, thickness,
					0, 0, 0, c);
		}
	} else {
		tinygfx_rectangle(&ud->img, x, y, w, h, filled, thickness, 0, 0, c);
	}
	return 0;
}

static int l_img_circle(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	int r = (int)luaL_checkinteger(L, 4);
	uint32_t c = (uint32_t)luaL_checkinteger(L, 5);
	bool filled = lua_toboolean(L, 6);
	int thickness = (int)luaL_optinteger(L, 7, 1);

	if (filled) {
		tinygfx_fill_circle(&ud->img, x, y, r, c);
	} else {
		tinygfx_circle(&ud->img, x, y, r, thickness, c);
	}
	return 0;
}

static int l_img_arc(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	int r = (int)luaL_checkinteger(L, 4);
	float a0 = (float)luaL_checknumber(L, 5);
	float a1 = (float)luaL_checknumber(L, 6);
	uint32_t c = (uint32_t)luaL_checkinteger(L, 7);

	// The arc family takes its options as a bundle. Defaults chosen to match
	// the common case: a plain stroked arc one pixel thick.
	arc_params_t p = {
		.thickness = (int)luaL_optinteger(L, 8, 1),
		.rounded = lua_toboolean(L, 9),
		.filled = lua_toboolean(L, 10),
		.sector = lua_toboolean(L, 11),
		.segment = lua_toboolean(L, 12),
		.dot1 = 0,
		.dot2 = 0,
		.resolution = 0,
		.color = c,
	};
	tinygfx_arc(&ud->img, x, y, r, a0, a1, &p);
	return 0;
}

static int l_img_triangle(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x0 = (int)luaL_checkinteger(L, 2);
	int y0 = (int)luaL_checkinteger(L, 3);
	int x1 = (int)luaL_checkinteger(L, 4);
	int y1 = (int)luaL_checkinteger(L, 5);
	int x2 = (int)luaL_checkinteger(L, 6);
	int y2 = (int)luaL_checkinteger(L, 7);
	uint32_t c = (uint32_t)luaL_checkinteger(L, 8);
	tinygfx_fill_triangle(&ud->img, x0, y0, x1, y1, x2, y2, c);
	return 0;
}

static int l_img_blit(lua_State *L) {
	img_ud_t *dst = check_img(L, 1);
	img_ud_t *src = check_img(L, 2);
	int x = (int)luaL_checkinteger(L, 3);
	int y = (int)luaL_checkinteger(L, 4);
	// -1 means no transparent colour, which is tinygfx's convention.
	int32_t transparent = (int32_t)luaL_optinteger(L, 5, -1);
	// NULL compose: straight copy with no alpha blending. Alpha maps need a
	// second buffer and a lifetime discussion, so they are left unbound until
	// something needs them.
	tinygfx_blit(&dst->img, &src->img, x, y, transparent, NULL);
	return 0;
}

// ------------------------------------------------------------- rendering ---

/*
 * Build a palette from a Lua table of 0xRRGGBB numbers.
 *
 * Indexed buffers store an index per pixel, and the palette maps those to
 * colours at render time. tinygfx's color_t also carries gradients and
 * pre-calculated ramps; only flat colours are exposed here, since a gradient
 * needs a lifetime discussion (its precalc array is owned elsewhere) and
 * nothing needs it yet.
 */
static int build_palette(lua_State *L, int idx, color_t *colors, int max) {
	if (lua_isnoneornil(L, idx)) {
		return 0;
	}
	luaL_checktype(L, idx, LUA_TTABLE);

	int n = (int)luaL_len(L, idx);
	if (n > max) {
		return luaL_error(L, "palette has %d entries, at most %d are used",
				n, max);
	}

	for (int i = 0; i < n; i++) {
		lua_rawgeti(L, idx, i + 1);
		if (!lua_isnumber(L, -1)) {
			lua_pop(L, 1);
			return luaL_error(L, "palette entry %d is not a number", i + 1);
		}
		memset(&colors[i], 0, sizeof(color_t));
		colors[i].type = COLOR_REGULAR;
		colors[i].color1 = (int)lua_tointeger(L, -1);
		colors[i].alpha = 255;
		lua_pop(L, 1);
	}

	return n;
}

/*
 * vesc.disp_render(img, x, y, [palette])
 *
 * Returns false when no driver is loaded rather than raising: a script that
 * runs on a board whose panel failed to initialise should be able to carry on
 * doing everything else.
 */
static int l_disp_render(lua_State *L) {
	img_ud_t *ud = check_img(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);

	color_t colors[16];
	memset(colors, 0, sizeof(colors));
	build_palette(L, 4, colors, 16);

	bool ok = disp_backend_render()(&ud->img, (uint16_t)x, (uint16_t)y, colors);
	lua_pushboolean(L, ok);
	return 1;
}

static int l_disp_clear(lua_State *L) {
	uint32_t c = (uint32_t)luaL_optinteger(L, 1, 0);
	disp_backend_clear()(c);
	return 0;
}

static int l_disp_reset(lua_State *L) {
	(void)L;
	disp_backend_reset()();
	return 0;
}

static int l_disp_loaded(lua_State *L) {
	lua_pushboolean(L, disp_backend_loaded());
	return 1;
}

// ------------------------------------------------------------ registration --

static const luaL_Reg img_methods[] = {
	{"dims", l_img_dims},
	{"clear", l_img_clear},
	{"setpix", l_img_setpix},
	{"getpix", l_img_getpix},
	{"line", l_img_line},
	{"rectangle", l_img_rectangle},
	{"circle", l_img_circle},
	{"arc", l_img_arc},
	{"triangle", l_img_triangle},
	{"blit", l_img_blit},
	{NULL, NULL},
};

static const luaL_Reg disp_fns[] = {
	{"img_buffer", l_img_buffer},
	{"disp_render", l_disp_render},
	{"disp_clear", l_disp_clear},
	{"disp_reset", l_disp_reset},
	{"disp_loaded", l_disp_loaded},
	{NULL, NULL},
};

void lua_vesc_disp_register(script_lua_t *s) {
	lua_State *L = script_lua_state(s);
	if (!L) {
		return;
	}

	/*
	 * Methods on the buffer itself, so drawing reads as buf:line(...) rather
	 * than vesc.img_line(buf, ...). That is the idiomatic shape in Lua and it
	 * keeps the vesc table from filling up with a dozen drawing calls.
	 */
	luaL_newmetatable(L, IMG_MT);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, img_methods, 0);
	lua_pop(L, 1);

	script_lua_register(s, disp_fns);
}
