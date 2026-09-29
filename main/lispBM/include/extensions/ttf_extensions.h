/*
  Copyright 2025 Joel Svensson              svenssonjoel@yahoo.se

  LispBM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LispBM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef TTF_EXTENSIONS_H_
#define TTF_EXTENSIONS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void lbm_ttf_extensions_init(void);

/* Measure a utf8 string in a font prepared by ttf-prepare, without going
 * through an extension call. font_size is the buffer length in bytes,
 * line_spacing a multiplier on the font's line height. Returns false if the
 * buffer is malformed or the string uses a glyph the font lacks. */
bool lbm_ttf_measure(const uint8_t *font, int32_t font_size,
                     const char *utf8, float line_spacing,
                     float *width, float *height);

#ifdef __cplusplus
}
#endif
#endif
