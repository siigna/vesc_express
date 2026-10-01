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

#ifndef MAIN_DISPLAY_TTF_FONT_H_
#define MAIN_DISPLAY_TTF_FONT_H_

#include <stdint.h>
#include <stdbool.h>

#include "tinygfx.h"

/*
 * Reading a prepared font, independent of script engine.
 *
 * These were static helpers inside the LispBM ttf extensions, which made
 * text the one drawing feature a non-lisp build could not have. They take no
 * interpreter types -- buffers, floats, and tinygfx's colour format -- so
 * they move out whole.
 *
 * A "prepared font" is what ttf-prepare produces: the glyphs already
 * rasterised at one size, with metrics and an optional kerning table, in one
 * blob. Rendering one therefore needs no TrueType parsing and no schrift,
 * which is why text can work on a build that cannot prepare fonts.
 */

// One kerning pair on disk: left codepoint, right codepoint, shift.
// Part of the font format, so it lives with the reader rather than with the
// writer in the lisp extensions.
#define FONT_KERN_PAIR_SIZE	(uint32_t)(4 + 4 + 4)

// Version and the index just past the preamble.
bool buffer_get_font_preamble(uint8_t *buffer, uint16_t *version, int32_t *index);

bool font_get_line_metrics(uint8_t *buffer, int32_t buffer_size,
		float *ascender, float *descender, float *line_gap, int32_t index);

bool font_get_kerning_table_index(uint8_t *buffer, int32_t buffer_size,
		int32_t *res_index, int32_t index);

bool font_get_glyphs_table_index(uint8_t *buffer, int32_t buffer_size,
		int32_t *res_index, uint32_t *num_codes, uint32_t *fmt, int32_t index);

/*
 * Look one glyph up by codepoint. gfx points into the font blob at the
 * glyph's pixels, in the format the glyph table reports -- an indexed image
 * whose values are coverage levels, not colours.
 */
bool font_get_glyph(uint8_t *buffer, float *advance_width,
		float *left_side_bearing, int32_t *y_offset, int32_t *width,
		int32_t *height, uint8_t **gfx, uint32_t utf32, uint32_t num_codes,
		color_format_t fmt, int32_t index);

bool font_get_kerning(uint8_t *buffer, uint32_t left, uint32_t right,
		float *x_shift, float *y_shift, int32_t index);

/*
 * One UTF-8 code point out of a NUL-terminated string, advancing next_ix.
 * False at the terminator or on a malformed sequence.
 *
 * The same decoder as get_utf32 in lispBM's ttf_backend.c, under a different
 * name rather than shared: that one sits behind a header which pulls in the
 * interpreter's display extensions, and the Lua font path is meant to need
 * nothing from the lispBM tree. Two copies of twenty lines of UTF-8 decoding
 * is the cheaper of those two problems, and this copy is the one with tests.
 */
bool ttf_font_utf32(const uint8_t *utf8, uint32_t *utf32, uint32_t ix,
		uint32_t *next_ix);

#endif /* MAIN_DISPLAY_TTF_FONT_H_ */
