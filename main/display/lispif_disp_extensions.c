/*
	Copyright 2023 Benjamin Vedder		benjamin@vedder.se
	Copyright 2023 Joel Svensson		svenssonjoel@yahoo.se
	Copyright 2023 Rasmus Söderhielm	rasmus.soderhielm@gmail.com
	Copyright 2025 Stephen Bouche

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


#include "lispif_disp_extensions.h"
#include "disp_backend.h"

/*
 * Registers a loaded driver with both the lisp interpreter's display
 * extensions and the engine-neutral registry in disp_backend.
 *
 * The callbacks were always engine-neutral -- image_buffer_t and color_t come
 * from tinygfx -- but the only way to register them lived inside the lisp
 * extension code, so a build without LispBM had no way to know a panel was
 * up. Recording them in both places keeps one source of truth for "which
 * display is loaded" whichever engine is built.
 */
static void set_display_callbacks(
		bool (*render)(image_buffer_t *img, uint16_t x, uint16_t y, color_t *colors),
		void (*clear)(uint32_t color),
		void (*reset)(void)) {
	lbm_display_extensions_set_callbacks(render, clear, reset);
	disp_backend_set(render, clear, reset);
}
#include "lispif.h"
#include "lbm_utils.h"
#include "lbm_custom_type.h"
#include "commands.h"
#include "utils.h"
#include "display/disp_sh8501b.h"
#include "display/disp_sh8601.h"
#include "display/disp_ili9341.h"
#include "display/disp_ssd1306.h"
#include "display/disp_st7789.h"
#include "display/disp_st7789a.h"
#include "display/disp_ili9488.h"
#include "display/disp_st7735.h"
#include "display/disp_ssd1351.h"
#include "display/disp_icna3306.h"
#include "display/disp_axs15231.h"
#include "display/disp_gc9a01.h"
#include "display/disp_jd9853.h"
#include "display/disp_st7701_rgb.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "display/disp_st7701.h"
#include "display/disp_jd9165.h"
#endif

#include <math.h>

// Display Drivers


static char *msg_invalid_gpio = "Invalid GPIO";
static char *msg_invalid_clk_speed = "Invalid clock speed";
static lbm_value ext_disp_load_sh8501b(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(5);

	int gpio_sd0, gpio_clk, gpio_cs, gpio_reset;
	gpio_sd0 = lbm_dec_as_i32(args[0]);
	gpio_clk = lbm_dec_as_i32(args[1]);
	gpio_cs = lbm_dec_as_i32(args[2]);
	gpio_reset = lbm_dec_as_i32(args[3]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	int spi_mhz = lbm_dec_as_i32(args[4]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_sh8501b_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, spi_mhz);

	set_display_callbacks(
			disp_sh8501b_render_image,
			disp_sh8501b_clear,
			disp_sh8501b_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_sh8601(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc;
	gpio_sd0 = lbm_dec_as_i32(args[0]);
	gpio_clk = lbm_dec_as_i32(args[1]);
	gpio_cs = lbm_dec_as_i32(args[2]);
	gpio_reset = lbm_dec_as_i32(args[3]);
	gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	int spi_mhz = lbm_dec_as_i32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_sh8601_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_sh8601_render_image,
			disp_sh8601_clear,
			disp_sh8601_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_ili9341(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc;
	gpio_sd0 = lbm_dec_as_i32(args[0]);
	gpio_clk = lbm_dec_as_i32(args[1]);
	gpio_cs = lbm_dec_as_i32(args[2]);
	gpio_reset = lbm_dec_as_i32(args[3]);
	gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_ili9341_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_ili9341_render_image,
			disp_ili9341_clear,
			disp_ili9341_reset);
	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_ssd1306(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(3);

	int gpio_sda = lbm_dec_as_i32(args[0]);
	int gpio_scl = lbm_dec_as_i32(args[1]);
	uint32_t clk_speed = lbm_dec_as_u32(args[2]);

	if (!utils_gpio_is_valid(gpio_sda) ||
			!utils_gpio_is_valid(gpio_scl)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	if (clk_speed > 8000000) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_ssd1306_init(gpio_sda, gpio_scl, clk_speed);
	set_display_callbacks(
			disp_ssd1306_render_image,
			disp_ssd1306_clear,
			disp_ssd1306_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_st7789(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0 = lbm_dec_as_i32(args[0]);
	int gpio_clk = lbm_dec_as_i32(args[1]);
	int gpio_cs = lbm_dec_as_i32(args[2]);
	int gpio_reset = lbm_dec_as_i32(args[3]);
	int gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			(!utils_gpio_is_valid(gpio_reset) && gpio_reset >= 0) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_st7789_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_st7789_render_image,
			disp_st7789_clear,
			disp_st7789_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_st7789a(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0  = lbm_dec_as_i32(args[0]);
	int gpio_clk  = lbm_dec_as_i32(args[1]);
	int gpio_cs   = lbm_dec_as_i32(args[2]);
	int gpio_reset = lbm_dec_as_i32(args[3]);
	int gpio_dc   = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			(gpio_cs >= 0 && !utils_gpio_is_valid(gpio_cs)) ||
			(!utils_gpio_is_valid(gpio_reset) && gpio_reset >= 0) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 80) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_st7789a_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_st7789a_render_image,
			disp_st7789a_clear,
			disp_st7789a_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_ili9488(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc;
	gpio_sd0 = lbm_dec_as_i32(args[0]);
	gpio_clk = lbm_dec_as_i32(args[1]);
	gpio_cs = lbm_dec_as_i32(args[2]);
	gpio_reset = lbm_dec_as_i32(args[3]);
	gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_ili9488_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_ili9488_render_image,
			disp_ili9488_clear,
			disp_ili9488_reset);
	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_st7735(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0 = lbm_dec_as_i32(args[0]);
	int gpio_clk = lbm_dec_as_i32(args[1]);
	int gpio_cs = lbm_dec_as_i32(args[2]);
	int gpio_reset = lbm_dec_as_i32(args[3]);
	int gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_st7735_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);
	set_display_callbacks(
			disp_st7735_render_image,
			disp_st7735_clear,
			disp_st7735_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_ssd1351(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0 = lbm_dec_as_i32(args[0]);
	int gpio_clk = lbm_dec_as_i32(args[1]);
	int gpio_cs = lbm_dec_as_i32(args[2]);
	int gpio_reset = lbm_dec_as_i32(args[3]);
	int gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_ssd1351_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_ssd1351_render_image,
			disp_ssd1351_clear,
			disp_ssd1351_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_icna3306(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(5);

	int gpio_sd0, gpio_clk, gpio_cs, gpio_reset;
	gpio_sd0 = lbm_dec_as_i32(args[0]);
	gpio_clk = lbm_dec_as_i32(args[1]);
	gpio_cs = lbm_dec_as_i32(args[2]);
	gpio_reset = lbm_dec_as_i32(args[3]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			!utils_gpio_is_valid(gpio_reset)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	int spi_mhz = lbm_dec_as_i32(args[4]);

	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_icna3306_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, spi_mhz);

	set_display_callbacks(
			disp_icna3306_render_image,
			disp_icna3306_clear,
			disp_icna3306_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_axs15231(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(8);

	int gpio_sd0 = lbm_dec_as_i32(args[0]);
	int gpio_sd1 = lbm_dec_as_i32(args[1]);
	int gpio_sd2 = lbm_dec_as_i32(args[2]);
	int gpio_sd3 = lbm_dec_as_i32(args[3]);
	int gpio_clk = lbm_dec_as_i32(args[4]);
	int gpio_cs = lbm_dec_as_i32(args[5]);
	int gpio_reset = lbm_dec_as_i32(args[6]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_sd1) ||
			!utils_gpio_is_valid(gpio_sd2) ||
			!utils_gpio_is_valid(gpio_sd3) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			(!utils_gpio_is_valid(gpio_reset) && gpio_reset >= 0)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[7]);
	if (spi_mhz == 0 || spi_mhz > 80) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_axs15231_init(gpio_sd0, gpio_sd1, gpio_sd2, gpio_sd3, gpio_clk, gpio_cs, gpio_reset, spi_mhz);

	set_display_callbacks(
			disp_axs15231_render_image,
			disp_axs15231_clear,
			disp_axs15231_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_gc9a01(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0 = lbm_dec_as_i32(args[0]);
	int gpio_clk = lbm_dec_as_i32(args[1]);
	int gpio_cs = lbm_dec_as_i32(args[2]);
	int gpio_reset = lbm_dec_as_i32(args[3]);
	int gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			(!utils_gpio_is_valid(gpio_reset) && gpio_reset >= 0) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);
	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_gc9a01_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_gc9a01_render_image,
			disp_gc9a01_clear,
			disp_gc9a01_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_jd9853(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(6);

	int gpio_sd0 = lbm_dec_as_i32(args[0]);
	int gpio_clk = lbm_dec_as_i32(args[1]);
	int gpio_cs = lbm_dec_as_i32(args[2]);
	int gpio_reset = lbm_dec_as_i32(args[3]);
	int gpio_dc = lbm_dec_as_i32(args[4]);

	if (!utils_gpio_is_valid(gpio_sd0) ||
			!utils_gpio_is_valid(gpio_clk) ||
			!utils_gpio_is_valid(gpio_cs) ||
			(!utils_gpio_is_valid(gpio_reset) && gpio_reset >= 0) ||
			!utils_gpio_is_valid(gpio_dc)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	uint32_t spi_mhz = lbm_dec_as_u32(args[5]);
	if (spi_mhz == 0 || spi_mhz > 40) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_jd9853_init(gpio_sd0, gpio_clk, gpio_cs, gpio_reset, gpio_dc, spi_mhz);

	set_display_callbacks(
			disp_jd9853_render_image,
			disp_jd9853_clear,
			disp_jd9853_reset);

	return ENC_SYM_TRUE;
}
#if CONFIG_IDF_TARGET_ESP32P4
static lbm_value ext_disp_load_jd9165(lbm_value *args, lbm_uint argn) {
	
	LBM_CHECK_ARGN_NUMBER(2);

	int pin_rst = lbm_dec_as_i32(args[0]);
	int lane_mbps = lbm_dec_as_i32(args[1]);

	if (pin_rst >= 0 && !utils_gpio_is_valid(pin_rst)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	if (lane_mbps <= 0) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	disp_jd9165_init(pin_rst, lane_mbps);

	set_display_callbacks(
			disp_jd9165_render_image,
			disp_jd9165_clear,
			disp_jd9165_reset);

	return ENC_SYM_TRUE;
}

static lbm_value ext_disp_load_st7701(lbm_value *args, lbm_uint argn) {
#if CONFIG_IDF_TARGET_ESP32P4
	LBM_CHECK_ARGN_NUMBER(2);

	int pin_rst = lbm_dec_as_i32(args[0]);
	int lane_mbps = lbm_dec_as_i32(args[1]);

	if (pin_rst >= 0 && !utils_gpio_is_valid(pin_rst)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}
	if (lane_mbps <= 0 || lane_mbps > 4000) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	if (!disp_st7701_init(pin_rst, lane_mbps)) {
		lbm_set_error_reason("Could not initialize the ST7701 display. "
				"Check the reset pin and the DSI lane rate.");
		return ENC_SYM_EERROR;
	}

	set_display_callbacks(
			disp_st7701_render_image,
			disp_st7701_clear,
			disp_st7701_reset);
	return ENC_SYM_TRUE;
#else
	(void)args;
	(void)argn;
	lbm_set_error_reason("ST7701 display is only available on ESP32P4");
	return ENC_SYM_EERROR;
#endif
}
#endif

// (disp-load-st7701-rgb width height cs sclk sda rst de vsync hsync pclk
//                       pclk-mhz '(d0 d1 ... d15))
//
// The data list is in bus order: B0..B4, G0..G5, R0..R4. A parallel RGB panel
// needs 16 data lines plus 4 sync lines plus a 3-wire SPI link for register
// setup, which is too many pins for a flat argument list.
static lbm_value ext_disp_load_st7701_rgb(lbm_value *args, lbm_uint argn) {
#if SOC_LCD_RGB_SUPPORTED
	if (argn != 12) {
		lbm_set_error_reason((char*)lbm_error_str_num_args);
		return ENC_SYM_EERROR;
	}
	for (int i = 0; i < 11; i++) {
		if (!lbm_is_number(args[i])) {
			lbm_set_error_reason((char*)lbm_error_str_no_number);
			return ENC_SYM_TERROR;
		}
	}

	disp_st7701_rgb_cfg_t cfg = {
		.width    = lbm_dec_as_i32(args[0]),
		.height   = lbm_dec_as_i32(args[1]),
		.pin_cs   = lbm_dec_as_i32(args[2]),
		.pin_sclk = lbm_dec_as_i32(args[3]),
		.pin_sda  = lbm_dec_as_i32(args[4]),
		.pin_rst  = lbm_dec_as_i32(args[5]),
		.pin_de    = lbm_dec_as_i32(args[6]),
		.pin_vsync = lbm_dec_as_i32(args[7]),
		.pin_hsync = lbm_dec_as_i32(args[8]),
		.pin_pclk  = lbm_dec_as_i32(args[9]),
		.pclk_hz   = lbm_dec_as_i32(args[10]) * 1000000,
	};

	if (cfg.width <= 0 || cfg.height <= 0) {
		lbm_set_error_reason("Invalid display size");
		return ENC_SYM_EERROR;
	}

	// The S3 cannot keep a wider bus fed from PSRAM at this resolution.
	if (cfg.pclk_hz <= 0 || cfg.pclk_hz > 30000000) {
		lbm_set_error_reason(msg_invalid_clk_speed);
		return ENC_SYM_EERROR;
	}

	int pins[5] = {cfg.pin_cs, cfg.pin_sclk, cfg.pin_sda,
			cfg.pin_de, cfg.pin_pclk};
	for (int i = 0; i < 5; i++) {
		if (!utils_gpio_is_valid(pins[i])) {
			lbm_set_error_reason(msg_invalid_gpio);
			return ENC_SYM_EERROR;
		}
	}
	// DE-mode panels leave hsync/vsync unused, and reset is often tied high
	if ((cfg.pin_rst >= 0 && !utils_gpio_is_valid(cfg.pin_rst)) ||
			(cfg.pin_vsync >= 0 && !utils_gpio_is_valid(cfg.pin_vsync)) ||
			(cfg.pin_hsync >= 0 && !utils_gpio_is_valid(cfg.pin_hsync))) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	lbm_value curr = args[11];
	int n = 0;
	while (lbm_is_cons(curr)) {
		lbm_value car = lbm_car(curr);
		if (!lbm_is_number(car)) {
			lbm_set_error_reason((char*)lbm_error_str_no_number);
			return ENC_SYM_TERROR;
		}
		if (n >= 16) {
			lbm_set_error_reason("Expected 16 data pins");
			return ENC_SYM_EERROR;
		}
		int pin = lbm_dec_as_i32(car);
		if (!utils_gpio_is_valid(pin)) {
			lbm_set_error_reason(msg_invalid_gpio);
			return ENC_SYM_EERROR;
		}
		cfg.pin_data[n++] = pin;
		curr = lbm_cdr(curr);
	}
	if (n != 16) {
		lbm_set_error_reason("Expected 16 data pins");
		return ENC_SYM_EERROR;
	}

	if (!disp_st7701_rgb_init(&cfg)) {
		lbm_set_error_reason("Could not initialize ST7701 RGB display");
		return ENC_SYM_EERROR;
	}

	set_display_callbacks(
			disp_st7701_rgb_render_image,
			disp_st7701_rgb_clear,
			disp_st7701_rgb_reset);
	return ENC_SYM_TRUE;
#else
	(void)args;
	(void)argn;
	lbm_set_error_reason("No parallel RGB LCD peripheral on this chip");
	return ENC_SYM_EERROR;
#endif
}

void lispif_load_disp_extensions(void) {

	lbm_display_extensions_init();

	lbm_add_extension("disp-load-sh8501b", ext_disp_load_sh8501b);
	lbm_add_extension("disp-load-sh8601", ext_disp_load_sh8601);
	lbm_add_extension("disp-load-ili9341", ext_disp_load_ili9341);
	lbm_add_extension("disp-load-ssd1306", ext_disp_load_ssd1306);
	lbm_add_extension("disp-load-st7789", ext_disp_load_st7789);
	lbm_add_extension("disp-load-st7789a", ext_disp_load_st7789a);
	lbm_add_extension("disp-load-ili9488", ext_disp_load_ili9488);
	lbm_add_extension("disp-load-st7735", ext_disp_load_st7735);
	lbm_add_extension("disp-load-ssd1351", ext_disp_load_ssd1351);
	lbm_add_extension("disp-load-icna3306", ext_disp_load_icna3306);
	lbm_add_extension("disp-load-axs15231", ext_disp_load_axs15231);
	lbm_add_extension("disp-load-gc9a01", ext_disp_load_gc9a01);
	lbm_add_extension("disp-load-jd9853", ext_disp_load_jd9853);

	#if SOC_LCD_RGB_SUPPORTED
	lbm_add_extension("disp-load-st7701-rgb", ext_disp_load_st7701_rgb);
	#endif

	#if CONFIG_IDF_TARGET_ESP32P4
	lbm_add_extension("disp-load-st7701", ext_disp_load_st7701);
	lbm_add_extension("disp-load-jd9165", ext_disp_load_jd9165);
	#endif
}

