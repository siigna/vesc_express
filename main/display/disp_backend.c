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

#include "disp_backend.h"

/*
 * volatile because a driver can be loaded from the script engine's task while
 * another task reads these -- the existing lisp code marked its equivalents
 * volatile for the same reason. A reader sees either the old or the new
 * pointer; both are callable, since the stubs are used in place of NULL.
 */
static bool (*volatile m_render)(image_buffer_t *img, uint16_t x, uint16_t y,
		color_t *colors) = NULL;
static void (*volatile m_clear)(uint32_t color) = NULL;
static void (*volatile m_reset)(void) = NULL;
static bool (*volatile m_orientation)(int rot) = NULL;
static volatile bool m_loaded = false;

static bool stub_render(image_buffer_t *img, uint16_t x, uint16_t y,
		color_t *colors) {
	(void)img; (void)x; (void)y; (void)colors;
	return false;
}

static void stub_clear(uint32_t color) {
	(void)color;
}

static void stub_reset(void) {
}

void disp_backend_set(disp_render_fn render, disp_clear_fn clear,
		disp_reset_fn reset) {
	m_render = render;
	m_clear = clear;
	m_reset = reset;
	// Cleared here rather than left over: loading a different panel must not
	// inherit the previous one's rotation handler.
	m_orientation = NULL;
	m_loaded = render != NULL;
}

void disp_backend_set_orientation_fn(disp_orientation_fn fn) {
	m_orientation = fn;
}

disp_orientation_fn disp_backend_orientation(void) {
	return m_orientation;
}

disp_render_fn disp_backend_render(void) {
	disp_render_fn f = m_render;
	return f ? f : stub_render;
}

disp_clear_fn disp_backend_clear(void) {
	disp_clear_fn f = m_clear;
	return f ? f : stub_clear;
}

disp_reset_fn disp_backend_reset(void) {
	disp_reset_fn f = m_reset;
	return f ? f : stub_reset;
}

bool disp_backend_loaded(void) {
	return m_loaded;
}
