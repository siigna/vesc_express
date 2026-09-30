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

#ifndef MAIN_DISPLAY_DISP_BACKEND_H_
#define MAIN_DISPLAY_DISP_BACKEND_H_

#include <stdint.h>
#include <stdbool.h>

#include "tinygfx.h"

/*
 * Which display driver is currently loaded, independent of script engine.
 *
 * The three callbacks a panel driver provides were already engine-neutral --
 * image_buffer_t and color_t come from tinygfx, not from LispBM -- but the
 * only way to register them was lbm_display_extensions_set_callbacks, which
 * lives inside the lisp interpreter's extension code. A Lua build excludes
 * that, and with it lost every display driver on the board.
 *
 * This is the registry both engines use. A driver, or a board's own
 * disp-init, calls disp_backend_set once the panel is up; whichever engine is
 * built reads the callbacks back out through here.
 */

typedef bool (*disp_render_fn)(image_buffer_t *img, uint16_t x, uint16_t y,
		color_t *colors);
typedef void (*disp_clear_fn)(uint32_t color);
typedef void (*disp_reset_fn)(void);

/*
 * Register the loaded driver. Any of the three may be NULL, in which case a
 * harmless stub is substituted -- a script calling into a display that failed
 * to initialise should do nothing, not crash the engine task.
 */
void disp_backend_set(disp_render_fn render, disp_clear_fn clear,
		disp_reset_fn reset);

// Never NULL: a stub is returned when nothing has been registered.
disp_render_fn disp_backend_render(void);
disp_clear_fn disp_backend_clear(void);
disp_reset_fn disp_backend_reset(void);

// Whether a real driver has been registered, for reporting rather than for
// guarding calls.
bool disp_backend_loaded(void);

#endif /* MAIN_DISPLAY_DISP_BACKEND_H_ */
