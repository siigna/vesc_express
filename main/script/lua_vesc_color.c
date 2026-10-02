/*
	Copyright 2026 Stephen Bouche

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
 * Colour bindings for the Lua engine, over display/color_core.c -- the same
 * arithmetic the lisp extensions now use, rather than a second copy.
 *
 * A colour is one packed number, WRGB from the top byte down, which is also
 * what the drawing bindings already take as a palette entry.
 *
 * On the width of that number: this build of Lua is LUA_32BITS, so a
 * lua_Integer is a signed 32-bit int and lua_Number is a 32-bit float.
 * Neither represents 0xFF000000 as a positive value, so a colour with a high
 * white channel comes back negative. That is the bit pattern, not a bug, and
 * it round-trips exactly through every binding here because they all read it
 * back as uint32. Only printing it looks odd. RGB colours, which are what a
 * dash uses, are at most 0xFFFFFF and stay positive.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "color_core.h"

// Accepts the same two spellings the lisp binding does: a float at or below
// 1.001 is a fraction, anything else is 0..255.
static uint8_t channel_arg(lua_State *L, int idx) {
	if (lua_isinteger(L, idx)) {
		return color_clamp8((int)lua_tointeger(L, idx));
	}

	return color_channel_from_float((float)luaL_checknumber(L, idx));
}

static uint32_t color_arg(lua_State *L, int idx) {
	// Through int32 first: a colour handed back from one of these is a signed
	// bit pattern, and going via lua_Number would round it.
	return (uint32_t)(int32_t)luaL_checkinteger(L, idx);
}

static void push_color(lua_State *L, uint32_t color) {
	lua_pushinteger(L, (lua_Integer)(int32_t)color);
}

// vesc.color_make(r, g, b [, w])
static int l_color_make(lua_State *L) {
	uint8_t r = channel_arg(L, 1);
	uint8_t g = channel_arg(L, 2);
	uint8_t b = channel_arg(L, 3);
	uint8_t w = lua_isnoneornil(L, 4) ? 0 : channel_arg(L, 4);

	push_color(L, color_pack(r, g, b, w));
	return 1;
}

/*
 * vesc.color_split(color) -> r, g, b, w
 *
 * Four returns rather than the list the lisp binding builds, and always four:
 * the lisp version takes a type argument choosing between three or four
 * components and integers or fractions, which is four shapes to remember.
 * Here the caller takes the values it wants and divides if it wants
 * fractions.
 */
static int l_color_split(lua_State *L) {
	uint32_t c = color_arg(L, 1);

	lua_pushinteger(L, COLOR_R(c));
	lua_pushinteger(L, COLOR_G(c));
	lua_pushinteger(L, COLOR_B(c));
	lua_pushinteger(L, COLOR_W(c));
	return 4;
}

// vesc.color_mix(color1, color2, ratio) -- ratio 0 is all color1, 1 all color2.
static int l_color_mix(lua_State *L) {
	uint32_t c1 = color_arg(L, 1);
	uint32_t c2 = color_arg(L, 2);
	float ratio = (float)luaL_checknumber(L, 3);

	push_color(L, color_core_mix(c1, c2, ratio));
	return 1;
}

static int l_color_add(lua_State *L) {
	push_color(L, color_core_add_sub(color_arg(L, 1), color_arg(L, 2), false));
	return 1;
}

static int l_color_sub(lua_State *L) {
	push_color(L, color_core_add_sub(color_arg(L, 1), color_arg(L, 2), true));
	return 1;
}

static int l_color_scale(lua_State *L) {
	push_color(L, color_core_scale(color_arg(L, 1), (float)luaL_checknumber(L, 2)));
	return 1;
}

static const luaL_Reg color_funcs[] = {
	{"color_make", l_color_make},
	{"color_split", l_color_split},
	{"color_mix", l_color_mix},
	{"color_add", l_color_add},
	{"color_sub", l_color_sub},
	{"color_scale", l_color_scale},
	{NULL, NULL},
};

void lua_vesc_color_register(script_lua_t *s) {
	script_lua_register(s, color_funcs);
}
