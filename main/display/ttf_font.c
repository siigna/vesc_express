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
 * Reading a prepared font. Moved out of the LispBM ttf extensions unchanged,
 * where being static made text the one drawing feature a non-lisp build could
 * not have. Nothing here touches an interpreter.
 */

#include "ttf_font.h"
#include "buffer.h"

#include <string.h>

/* cppcheck-suppress constParameterPointer ; signature kept identical to the
 * lisp ttf extensions that call it -- narrowing it here would mean editing the
 * vendored caller as well, which is divergence for a style note. */
bool buffer_get_font_preamble(uint8_t* buffer, uint16_t *version, int32_t *index) {

  uint16_t zero = buffer_get_uint16(buffer, index);
  if (zero == 0) {
    *version = buffer_get_uint16(buffer, index);
    if (strncmp((const char *)&buffer[*index], "font", 4) == 0) {
      *index += (int32_t)sizeof("font"); // includes 0 for constant string
      return true;
    }
  }
  return false;
}

bool font_get_line_metrics(uint8_t *buffer, int32_t buffer_size, float *ascender, float *descender, float *line_gap ,int32_t index) {

  while(index < buffer_size) {
    const char *str = (const char*)&buffer[index];
    if (strncmp(str, "lmtx", 4) == 0) {
      int32_t i = index + 5 + 4; // skip over string and size field;
      *ascender = buffer_get_float32_auto(buffer, &i);
      *descender = buffer_get_float32_auto(buffer, &i);
      *line_gap = buffer_get_float32_auto(buffer, &i);
      return true;
    }
    index += (int32_t)(strlen(str) + 1);
    index += (int32_t)buffer_get_uint32(buffer,&index); // just to next position
  }
  return false;
}

bool font_get_kerning_table_index(uint8_t *buffer, int32_t buffer_size, int32_t *res_index, int32_t index) {

  while (index < buffer_size) {
    const char *str = (const char*)&buffer[index];
    if (strncmp(str, "kern", 4) == 0) {
      *res_index = index + 5 + 4;
      return true;
    }
    index += (int32_t)(strlen(str) + 1);
    index += (int32_t)buffer_get_uint32(buffer,&index); // jump to next position
  }
  return false;
}

bool font_get_glyphs_table_index(uint8_t *buffer, int32_t buffer_size, int32_t *res_index, uint32_t *num_codes, uint32_t *fmt, int32_t index) {
  while (index < buffer_size) {
    const char *str = (const char*)&buffer[index];
    if (strncmp(str, "glyphs", 6) == 0) {
      int32_t i = index + 7 + 4;
      *num_codes = buffer_get_uint32(buffer,&i);
      *fmt = buffer_get_uint32(buffer,&i);
      *res_index = i;
      return true;
    }
    index += (int32_t)(strlen(str) + 1);
    index += (int32_t)buffer_get_uint32(buffer,&index);
  }
  return false;
}

bool font_get_glyph(uint8_t *buffer,
                           float *advance_width,
                           float *left_side_bearing,
                           int32_t *y_offset,
                           int32_t *width,
                           int32_t *height,
                           uint8_t **gfx,
                           uint32_t utf32,
                           uint32_t num_codes,
                           color_format_t fmt,
                           int32_t index) {

  uint32_t i = 0;
  while (i < num_codes) {
    uint32_t c = buffer_get_uint32(buffer, &index);
    if (c == utf32) {
      *advance_width = buffer_get_float32_auto(buffer, &index);
      *left_side_bearing = buffer_get_float32_auto(buffer, &index);
      *y_offset = buffer_get_int32(buffer, &index);
      *width = buffer_get_int32(buffer, &index);
      *height = buffer_get_int32(buffer,&index);
      *gfx = &buffer[index];
      return true;
    } else {
      index += 12;
      int32_t w = buffer_get_int32(buffer, &index);
      int32_t h = buffer_get_int32(buffer, &index);
      index += (int32_t)image_dims_to_size_bytes(fmt, (uint16_t)w, (uint16_t)h);
    }
    i++;
  }
  return false;
}

/* cppcheck-suppress constParameterPointer ; see the note above. */
bool font_get_kerning(uint8_t *buffer, uint32_t left, uint32_t right, float *x_shift, float *y_shift, int32_t index) {

  uint32_t num_rows = buffer_get_uint32(buffer, &index);

  for (uint32_t row = 0; row < num_rows; row ++) {

    uint32_t row_code = buffer_get_uint32(buffer, &index);
    uint32_t row_len  = buffer_get_uint32(buffer, &index);

    if (row_code == left) {
      for (uint32_t col = 0; col < row_len; col ++) {
        uint32_t col_code = buffer_get_uint32(buffer, &index);
        if (col_code == right) {
          *x_shift = buffer_get_float32_auto(buffer, &index);
          *y_shift = buffer_get_float32_auto(buffer, &index);
          return true;
        } else {
          index += 8;
        }
      }
    } else {
      index += (int32_t)(row_len * FONT_KERN_PAIR_SIZE);
    }
  }
  return false;
}

bool ttf_font_utf32(const uint8_t *utf8, uint32_t *utf32, uint32_t ix,
		uint32_t *next_ix) {
	const uint8_t *u = &utf8[ix];
	uint32_t c = 0;

	if (u[0] == 0) {
		return false;
	}

	if (!(u[0] & 0x80U)) {
		*utf32 = u[0];
		*next_ix = ix + 1;
	} else if ((u[0] & 0xe0U) == 0xc0U) {
		c = (uint32_t)(u[0] & 0x1fU) << 6;
		if ((u[1] & 0xc0U) != 0x80U) return false;
		*utf32 = c + (u[1] & 0x3fU);
		*next_ix = ix + 2;
	} else if ((u[0] & 0xf0U) == 0xe0U) {
		c = (uint32_t)(u[0] & 0x0fU) << 12;
		if ((u[1] & 0xc0U) != 0x80U) return false;
		c += (uint32_t)(u[1] & 0x3fU) << 6;
		if ((u[2] & 0xc0U) != 0x80U) return false;
		*utf32 = c + (u[2] & 0x3fU);
		*next_ix = ix + 3;
	} else if ((u[0] & 0xf8U) == 0xf0U) {
		c = (uint32_t)(u[0] & 0x07U) << 18;
		if ((u[1] & 0xc0U) != 0x80U) return false;
		c += (uint32_t)(u[1] & 0x3fU) << 12;
		if ((u[2] & 0xc0U) != 0x80U) return false;
		c += (uint32_t)(u[2] & 0x3fU) << 6;
		if ((u[3] & 0xc0U) != 0x80U) return false;
		c += (u[3] & 0x3fU);
		// Surrogates are not scalar values, and no font has a glyph for one.
		if ((c & 0xFFFFF800U) == 0xD800U) return false;
		*utf32 = c;
		*next_ix = ix + 4;
	} else {
		return false;
	}

	return true;
}
