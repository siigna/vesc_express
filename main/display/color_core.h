/*
	Copyright 2025

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
 * Colour arithmetic, with no script engine in it.
 *
 * A colour is one packed 32-bit value, WRGB from the top byte down -- not an
 * object. These were static functions inside lbm_color_extensions.c, which
 * meant the Lua engine could not reach them; both engines now bind to this.
 *
 * Nothing here allocates, touches hardware or depends on an interpreter, so
 * the host tests cover it directly.
 */

#ifndef COLOR_CORE_H_
#define COLOR_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#define COLOR_W(c)	(((c) >> 24) & 0xFF)
#define COLOR_R(c)	(((c) >> 16) & 0xFF)
#define COLOR_G(c)	(((c) >> 8) & 0xFF)
#define COLOR_B(c)	((c) & 0xFF)

// Clamp to 0..255. Separate from the pack so a caller that already has bytes
// does not pay for it.
uint8_t color_clamp8(int v);

/*
 * A channel given as a float is taken as a 0..1 fraction when it is at most
 * 1.001, and as 0..255 above that. That rule comes from the lisp binding and
 * is kept because scripts rely on both spellings -- 1.0 means full, and so
 * does 255.
 */
uint8_t color_channel_from_float(float v);

uint32_t color_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t w);

// ratio is clamped to 0..1; 0 is all of color1, 1 is all of color2.
uint32_t color_core_mix(uint32_t color1, uint32_t color2, float ratio);

// Per channel, clamped at both ends rather than wrapping.
uint32_t color_core_add_sub(uint32_t color1, uint32_t color2, bool sub);

uint32_t color_core_scale(uint32_t color, float scale);

#endif
