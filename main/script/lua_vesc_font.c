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
 * Text for the Lua engine.
 *
 * Draws fonts prepared by ttf-prepare: the glyphs are already rasterised at
 * one size, with metrics and an optional kerning table, in a single blob. So
 * rendering needs no TrueType parsing and no schrift -- only the readers in
 * display/ttf_font.c -- which is why text works on a build that cannot
 * prepare a font.
 *
 * Prepare fonts on a host, or on a lisp build, and bundle the result with
 * luapack.py; a script reaches it through require. Preparing on device is not
 * bound, because schrift allocates through LispBM's memory pool and dragging
 * that into a Lua build to generate something a build step can generate is
 * the wrong trade.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "ttf_font.h"
#include "tinygfx.h"

#include <string.h>

#define FONT_MT		"vesc.font"
#define IMG_MT		"vesc.img"

/*
 * A font is the blob plus the offsets found in it once, at load, rather than
 * re-scanned per glyph. Scanning is a walk over a tagged section list, and a
 * dash redrawing a few fields at 10 Hz would otherwise do it hundreds of
 * times a second for no gain.
 */
typedef struct {
	const uint8_t *data;	// into a Lua string pinned in the registry
	int32_t size;
	int32_t glyphs_index;
	int32_t kern_index;
	uint32_t num_codes;
	uint32_t fmt;
	bool has_kern;
	float ascender;
	float descender;
	float line_gap;
	int ref;		// registry reference keeping the string alive
} font_ud_t;

// Mirrors the layout lua_vesc_disp.c allocates, so a buffer can be shared
// between the two without either owning the other's header.
typedef struct {
	image_buffer_t img;
} img_ud_t;

static font_ud_t *check_font(lua_State *L, int idx) {
	return (font_ud_t *)luaL_checkudata(L, idx, FONT_MT);
}

static int l_font_gc(lua_State *L) {
	font_ud_t *f = (font_ud_t *)luaL_checkudata(L, 1, FONT_MT);
	if (f->ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, f->ref);
		f->ref = LUA_NOREF;
	}
	f->data = NULL;
	return 0;
}

/*
 * vesc.font_load(blob)
 *
 * blob is the prepared font as a string, typically from require of a bundled
 * module. The string is pinned in the registry and the font points into it:
 * a prepared font is tens of kilobytes and copying it would double that
 * against the engine's memory ceiling for no benefit, since Lua strings are
 * immutable.
 */
static int l_font_load(lua_State *L) {
	size_t len = 0;
	const char *blob = luaL_checklstring(L, 1, &len);

	if (len < 16) {
		return luaL_error(L, "font_load: %d bytes is too small to be a "
				"prepared font", (int)len);
	}

	font_ud_t *f = (font_ud_t *)lua_newuserdatauv(L, sizeof(font_ud_t), 0);
	memset(f, 0, sizeof(*f));
	f->ref = LUA_NOREF;
	f->data = (const uint8_t *)blob;
	f->size = (int32_t)len;

	uint8_t *buf = (uint8_t *)f->data;
	int32_t index = 0;
	uint16_t version = 0;

	if (!buffer_get_font_preamble(buf, &version, &index)) {
		return luaL_error(L, "font_load: not a prepared font (bad preamble). "
				"Prepare it with ttf-prepare and bundle the result.");
	}

	if (!font_get_line_metrics(buf, f->size, &f->ascender, &f->descender,
			&f->line_gap, index)) {
		return luaL_error(L, "font_load: no line metrics in this font");
	}

	if (!font_get_glyphs_table_index(buf, f->size, &f->glyphs_index,
			&f->num_codes, &f->fmt, index)) {
		return luaL_error(L, "font_load: no glyph table in this font");
	}

	// Kerning is optional; a font prepared without it simply has no table.
	f->has_kern = font_get_kerning_table_index(buf, f->size, &f->kern_index,
			index);

	luaL_setmetatable(L, FONT_MT);

	// Pin the string. Done after the userdata exists so that an error above
	// cannot leave a reference behind.
	lua_pushvalue(L, 1);
	f->ref = luaL_ref(L, LUA_REGISTRYINDEX);

	return 1;
}

static int l_font_metrics(lua_State *L) {
	font_ud_t *f = check_font(L, 1);
	lua_pushnumber(L, (lua_Number)f->ascender);
	lua_pushnumber(L, (lua_Number)f->descender);
	lua_pushnumber(L, (lua_Number)f->line_gap);
	lua_pushinteger(L, (lua_Integer)f->num_codes);
	return 4;
}

/*
 * Walk a utf8 string, handing each glyph to a callback. Shared by measuring
 * and drawing so the two cannot disagree about advance widths or kerning --
 * text that measures differently from how it draws is how a layout ends up
 * clipping its own last character.
 */
typedef void (*glyph_fn)(void *ctx, float x, int32_t y_off, int32_t w,
		int32_t h, uint8_t *gfx);

static bool walk_text(font_ud_t *f, const char *utf8, glyph_fn fn, void *ctx,
		float *width_out) {
	float x = 0.0f;
	uint32_t prev = 0;
	uint32_t i = 0;
	uint32_t next = 0;
	uint32_t code = 0;

	while (ttf_font_utf32((uint8_t *)utf8, &code, i, &next)) {
		i = next;

		float advance = 0.0f;
		float lsb = 0.0f;
		int32_t y_off = 0, w = 0, h = 0;
		uint8_t *gfx = NULL;

		if (!font_get_glyph((uint8_t *)f->data, &advance, &lsb, &y_off, &w, &h,
				&gfx, code, f->num_codes, (color_format_t)f->fmt,
				f->glyphs_index)) {
			// A missing glyph is skipped rather than failing the whole
			// string: one unexpected character should not blank a field.
			prev = code;
			continue;
		}

		if (f->has_kern && prev != 0) {
			float kx = 0.0f, ky = 0.0f;
			if (font_get_kerning((uint8_t *)f->data, prev, code, &kx, &ky,
					f->kern_index)) {
				x += kx;
			}
		}

		if (fn) {
			fn(ctx, x + lsb, y_off, w, h, gfx);
		}

		x += advance;
		prev = code;
	}

	if (width_out) {
		*width_out = x;
	}
	return true;
}

static int l_font_measure(lua_State *L) {
	font_ud_t *f = check_font(L, 1);
	const char *str = luaL_checkstring(L, 2);

	float w = 0.0f;
	walk_text(f, str, NULL, NULL, &w);

	/*
	 * Truncated, like the lisp's ttf-text-dims, which returns (uint32_t)w.
	 *
	 * This is the one that matters: every centring calculation in the dash is
	 * (buffer_width - text_width) / 2, so a width one larger moves the text a
	 * pixel left. Rounding here was the whole of the remaining difference
	 * between the ported view_static and its reference -- 1105 pixels, all of
	 * it text shifted by one.
	 *
	 * Height keeps its rounding: the lisp truncates a different quantity
	 * there and the two agree on the fonts in use, so changing it would be a
	 * guess rather than a match.
	 */
	lua_pushinteger(L, (lua_Integer)w);
	lua_pushinteger(L, (lua_Integer)(f->ascender - f->descender + 0.5f));
	return 2;
}

typedef struct {
	image_buffer_t *dst;
	int x;
	int y;
	uint32_t colour;
	bool antialias;
	uint32_t levels;
	color_format_t glyph_fmt;
} draw_ctx_t;

static void draw_glyph(void *vctx, float gx, int32_t y_off, int32_t w,
		int32_t h, uint8_t *gfx) {
	draw_ctx_t *c = (draw_ctx_t *)vctx;

	// The glyph is an indexed image whose values are coverage, not colour, so
	// it is read through a temporary header rather than blitted directly.
	image_buffer_t g = {
		.fmt = c->glyph_fmt,
		.width = (uint16_t)w,
		.height = (uint16_t)h,
		.data = gfx,
		.mem_base = gfx,
	};

	for (int32_t yy = 0; yy < h; yy++) {
		for (int32_t xx = 0; xx < w; xx++) {
			uint32_t cov = getpixel(&g, (int)xx, (int)yy);
			if (cov == 0) {
				continue;
			}
			/*
			 * Coverage maps onto consecutive palette entries starting at the
			 * requested colour, so a three-level glyph can use a three-step
			 * ramp for antialiasing. Without that, every non-zero coverage
			 * draws the same colour and the text is simply hard-edged --
			 * which is the right default for small readouts.
			 */
			uint32_t px = c->antialias && c->levels > 1
					? c->colour + (cov - 1)
					: c->colour;
			/*
			 * Truncated, as the lisp does with (int)(x_n +
			 * left_side_bearing). Rounding puts a glyph whose position lands
			 * on a half pixel one to the right of where the lisp puts it,
			 * which is a per-glyph difference rather than a whole-string
			 * shift: in "HIGH" only the G moved.
			 */
			putpixel(c->dst, c->x + (int)gx + (int)xx,
					c->y + (int)y_off + (int)yy, px);
		}
	}
}

/*
 * buf:text(x, y, font, str, colour, [antialias])
 *
 * y is the baseline, not the top: that is what the font's metrics are
 * relative to, and placing text by its top makes lines of different heights
 * fail to line up.
 */
static int l_img_text(lua_State *L) {
	img_ud_t *ud = (img_ud_t *)luaL_checkudata(L, 1, IMG_MT);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	font_ud_t *f = check_font(L, 4);
	const char *str = luaL_checkstring(L, 5);
	uint32_t colour = (uint32_t)luaL_checkinteger(L, 6);
	bool aa = lua_toboolean(L, 7);

	uint32_t levels = 1;
	switch ((color_format_t)f->fmt) {
	case indexed2: levels = 1; break;
	case indexed4: levels = 3; break;
	case indexed16: levels = 15; break;
	default: levels = 1; break;
	}

	/*
	 * Refuse a base index whose coverage range runs off the end of the
	 * destination, rather than writing past it.
	 *
	 * Antialiasing adds coverage to the base index, so an indexed4 glyph
	 * drawn at index 3 writes 3, 4 and 5 -- and an indexed4 buffer holds
	 * 0..3. The pixels past the end are the solid centre of every glyph, so
	 * it does not look like an error: it looks like text that failed to
	 * antialias, which is a bad thing to debug by eye. Checked against the
	 * destination format rather than the palette because the palette is not
	 * supplied until disp_render, by which time the pixels are already wrong.
	 */
	if (aa && levels > 1) {
		uint32_t dst_max = 0;
		switch (ud->img.fmt) {
		case indexed2: dst_max = 1; break;
		case indexed4: dst_max = 3; break;
		case indexed16: dst_max = 15; break;
		default: dst_max = 0; break;	// rgb and friends index nothing
		}

		if (dst_max > 0 && colour + levels - 1 > dst_max) {
			return luaL_error(L, "text: base index %d with %d antialias "
					"levels reaches %d, past the %d this buffer holds. "
					"Use %d for a full ramp, or draw without antialiasing.",
					(int)colour, (int)levels, (int)(colour + levels - 1),
					(int)dst_max, (int)(dst_max - levels + 1));
		}
	}

	draw_ctx_t ctx = {
		.dst = &ud->img,
		.x = x,
		.y = y,
		.colour = colour,
		.antialias = aa,
		.levels = levels,
		.glyph_fmt = (color_format_t)f->fmt,
	};

	float w = 0.0f;
	walk_text(f, str, draw_glyph, &ctx, &w);

	/*
	 * Truncated, not rounded, to match the lisp: ttf-text-dims returns
	 * (uint32_t)w, and the dash's centring was tuned against that.
	 *
	 * Changing it did not move the one-pixel text offset still outstanding
	 * between the ported view_static and its reference -- those widths were
	 * already integral -- so this is a semantics match rather than that fix.
	 */
	lua_pushinteger(L, (lua_Integer)w);
	return 1;
}

/*
 * font:glyph_dims(str) -> width, height of the first glyph in str
 *
 * The rasterised size of one glyph, which is not the same as what measure()
 * reports: that is an advance width, and includes the side bearings a layout
 * needs but a centring calculation must not.
 *
 * It exists because that is how the dash centres text vertically -- by the
 * cap height of "D" rather than by the font's ascent, so a line of digits
 * sits where the eye expects. Without it a port cannot place text where the
 * lisp places it, which is how this binding came to be written: the
 * view_static render differed from its reference and this was missing.
 */
static int l_font_glyph_dims(lua_State *L) {
	font_ud_t *f = check_font(L, 1);
	const char *str = luaL_checkstring(L, 2);

	uint32_t utf32 = 0;
	uint32_t next_i = 0;
	if (!ttf_font_utf32((const uint8_t *)str, &utf32, 0, &next_i)) {
		return luaL_error(L, "glyph_dims: no character to measure");
	}

	float advance_width = 0.0f;
	float left_side_bearing = 0.0f;
	int32_t y_offset = 0;
	int32_t width = 0;
	int32_t height = 0;
	uint8_t *gfx = NULL;

	if (!font_get_glyph((uint8_t *)f->data, &advance_width, &left_side_bearing,
			&y_offset, &width, &height, &gfx, utf32, f->num_codes,
			(color_format_t)f->fmt, f->glyphs_index)) {
		// A font prepared with a reduced character set is the normal case
		// here, so a missing glyph is zero sized rather than an error.
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
		return 2;
	}

	lua_pushinteger(L, width);
	lua_pushinteger(L, height);
	return 2;
}

static const luaL_Reg font_methods[] = {
	{"metrics", l_font_metrics},
	{"measure", l_font_measure},
	{"glyph_dims", l_font_glyph_dims},
	{NULL, NULL},
};

static const luaL_Reg font_fns[] = {
	{"font_load", l_font_load},
	{NULL, NULL},
};

void lua_vesc_font_register(script_lua_t *s) {
	lua_State *L = script_lua_state(s);
	if (!L) {
		return;
	}

	luaL_newmetatable(L, FONT_MT);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, font_methods, 0);
	lua_pushcfunction(L, l_font_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);

	// text() goes on the image metatable, next to the other drawing methods,
	// so it reads as buf:text(...) like buf:line(...).
	luaL_getmetatable(L, IMG_MT);
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, l_img_text);
		lua_setfield(L, -2, "text");
	}
	lua_pop(L, 1);

	script_lua_register(s, font_fns);
}
