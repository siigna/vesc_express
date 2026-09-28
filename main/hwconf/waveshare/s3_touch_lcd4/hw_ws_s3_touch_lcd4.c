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

#include "hw_ws_s3_touch_lcd4.h"

#include "lispif.h"
#include "lispbm.h"
#include "lispif_disp_extensions.h"
#include "extensions/display_extensions.h"
#include "disp_st7701_rgb.h"

// The panel needs 20 GPIOs for the bus plus 3 for register setup, so the
// pin map lives here rather than in the package. A dash package only calls
// (disp-init), the way the Dash16 package does.
static lbm_value ext_disp_init(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;

	static const int data_pins[16] = DISP_DATA_PINS;

	disp_st7701_rgb_cfg_t cfg = {
		.width    = DISP_WIDTH,
		.height   = DISP_HEIGHT,
		.pin_cs   = DISP_CS,
		.pin_sclk = DISP_SCLK,
		.pin_sda  = DISP_SDA,
		.pin_rst  = DISP_RST,
		.pin_de    = DISP_DE,
		.pin_vsync = DISP_VSYNC,
		.pin_hsync = DISP_HSYNC,
		.pin_pclk  = DISP_PCLK,
		.pclk_hz   = DISP_PCLK_HZ,
	};
	for (int i = 0; i < 16; i++) {
		cfg.pin_data[i] = data_pins[i];
	}

	if (!disp_st7701_rgb_init(&cfg)) {
		lbm_set_error_reason("Could not initialize the display");
		return ENC_SYM_EERROR;
	}

	lbm_display_extensions_set_callbacks(
			disp_st7701_rgb_render_image,
			disp_st7701_rgb_clear,
			disp_st7701_rgb_reset);

	return ENC_SYM_TRUE;
}

// The GT911 loader is generic, so rather than wrap it the board just reports
// its pins and the package passes them straight through:
//   (apply touch-load-gt911 (touch-pins))
// -> (sda scl rst int width height)
static lbm_value ext_touch_pins(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;

	lbm_value res = ENC_SYM_NIL;
	const int vals[6] = {
		TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT, DISP_WIDTH, DISP_HEIGHT
	};

	// Built back to front, since cons prepends
	for (int i = 5; i >= 0; i--) {
		res = lbm_cons(lbm_enc_i(vals[i]), res);
		if (lbm_is_error(res)) {
			return res;
		}
	}

	return res;
}

static void load_extensions(bool main_found) {
	if (main_found) {
		return;
	}

	lbm_add_extension("disp-init", ext_disp_init);
	lbm_add_extension("touch-pins", ext_touch_pins);
}

void hw_init(void) {
	lispif_add_ext_load_callback(load_extensions);
}
