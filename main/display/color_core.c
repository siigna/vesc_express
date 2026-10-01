/*
	Copyright 2025

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

#include "color_core.h"

// Local rather than utils_truncate_number, so this file depends on nothing
// and the host tests can build it alone.
static float clampf(float v, float lo, float hi) {
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

uint8_t color_clamp8(int v) {
	if (v < 0) {
		return 0;
	}
	if (v > 255) {
		return 255;
	}
	return (uint8_t)v;
}

uint8_t color_channel_from_float(float v) {
	if (v < 1.001f) {
		v *= 255.0f;
	}

	return color_clamp8((int)v);
}

uint32_t color_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
	return ((uint32_t)w << 24) | ((uint32_t)r << 16) |
			((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t color_core_mix(uint32_t color1, uint32_t color2, float ratio) {
	ratio = clampf(ratio, 0.0f, 1.0f);
	const float inv = 1.0f - ratio;

	return color_pack(
			(uint8_t)((float)COLOR_R(color1) * inv + (float)COLOR_R(color2) * ratio),
			(uint8_t)((float)COLOR_G(color1) * inv + (float)COLOR_G(color2) * ratio),
			(uint8_t)((float)COLOR_B(color1) * inv + (float)COLOR_B(color2) * ratio),
			(uint8_t)((float)COLOR_W(color1) * inv + (float)COLOR_W(color2) * ratio));
}

uint32_t color_core_add_sub(uint32_t color1, uint32_t color2, bool sub) {
	const int sign = sub ? -1 : 1;

	return color_pack(
			color_clamp8((int)COLOR_R(color1) + sign * (int)COLOR_R(color2)),
			color_clamp8((int)COLOR_G(color1) + sign * (int)COLOR_G(color2)),
			color_clamp8((int)COLOR_B(color1) + sign * (int)COLOR_B(color2)),
			color_clamp8((int)COLOR_W(color1) + sign * (int)COLOR_W(color2)));
}

uint32_t color_core_scale(uint32_t color, float scale) {
	return color_pack(
			(uint8_t)clampf((float)COLOR_R(color) * scale, 0.0f, 255.0f),
			(uint8_t)clampf((float)COLOR_G(color) * scale, 0.0f, 255.0f),
			(uint8_t)clampf((float)COLOR_B(color) * scale, 0.0f, 255.0f),
			(uint8_t)clampf((float)COLOR_W(color) * scale, 0.0f, 255.0f));
}
