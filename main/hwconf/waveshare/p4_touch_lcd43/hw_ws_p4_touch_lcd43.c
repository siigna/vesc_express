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

#include "hw_ws_p4_touch_lcd43.h"

#include "driver/gpio.h"

/* No lisp extensions: the display and touch controllers on this board are
 * both driven by loaders that already exist (disp-load-st7701,
 * touch-load-gt911), and the backlight is a plain PWM pin, so unlike the S3
 * Touch LCD 4 there is no pin map to hide behind a disp-init.
 *
 * The one thing that does have to happen in C is parking the backlight. It is
 * active-LOW, so between reset and the point where the lisp app drives it the
 * pin floats and the panel lights up showing uninitialised memory. hw_init
 * runs before lispif_init, so switching it off here means the screen stays
 * dark until something is actually drawn. */
void hw_init(void) {
	gpio_config_t bl_cfg = {
		.pin_bit_mask = 1ULL << DISP_BACKLIGHT,
		.mode         = GPIO_MODE_OUTPUT,
		.pull_up_en   = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	gpio_config(&bl_cfg);
	gpio_set_level(DISP_BACKLIGHT, DISP_BACKLIGHT_OFF_LEVEL);
}
