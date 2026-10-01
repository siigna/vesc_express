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

/*
 * LispBM bindings for the touch core.
 *
 * The hardware lives in touch_core.c, which has no interpreter in it. What is
 * left here is argument checking with lisp error reasons, value construction,
 * and flattening a touch event into the interpreter's event queue.
 */

#include "lispif_touch_extensions.h"

#include "touch_core.h"
#include "lispif_events.h"
#include "lbm_vesc_utils.h"
#include "eval_cps.h"
#include "extensions.h"
#include "heap.h"
#include "lbm_flat_value.h"
#include "utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_touch.h"

#define TOUCH_I2C_DEFAULT_FREQ 400000

static char *msg_invalid_gpio = "Invalid GPIO";
static char *msg_invalid_size = "Invalid touch size";
static char *msg_invalid_i2c_speed = "Invalid I2C speed";
static char *msg_invalid_spi_host = "Invalid SPI host";
static char *msg_touch_not_loaded = "Touch not loaded";
static char *msg_touch_runtime = "Touch runtime init failed";

static lbm_uint sym_cst816s = 0;
static lbm_uint sym_gt911 = 0;
static lbm_uint sym_cst9217 = 0;
static lbm_uint sym_xpt2046 = 0;
static lbm_uint sym_axs15231 = 0;
static lbm_uint sym_cst836u = 0;

static bool start_flatten_with_gc(lbm_flat_value_t *v, size_t buffer_size) {
	if (lbm_start_flatten(v, buffer_size)) {
		return true;
	}

	int timeout = 3;
	uint32_t gc_last = lbm_heap_state.gc_num;
	lbm_request_gc();

	while (lbm_heap_state.gc_num <= gc_last && timeout > 0) {
		vTaskDelay(1);
		timeout--;
	}

	return lbm_start_flatten(v, buffer_size);
}

// The core reports which part produced an event as an enum; lisp has always
// reported it as the symbol the script loaded it by.
static lbm_uint touch_part_symbol(touch_part_t part) {
	switch (part) {
	case TOUCH_PART_CST816S: return sym_cst816s;
	case TOUCH_PART_GT911: return sym_gt911;
	case TOUCH_PART_CST9217: return sym_cst9217;
	case TOUCH_PART_XPT2046: return sym_xpt2046;
	case TOUCH_PART_AXS15231: return sym_axs15231;
	case TOUCH_PART_CST836U: return sym_cst836u;
	default: return 0;
	}
}

static bool touch_events_enabled(void) {
	return event_touch_int_en;
}

static void touch_emit_event(bool pressed, const touch_point_t *point, touch_part_t part) {
	lbm_flat_value_t flat;
	if (!start_flatten_with_gc(&flat, 96)) {
		return;
	}

	f_cons(&flat);
	f_sym(&flat, sym_event_touch_int);

	f_cons(&flat);
	f_sym(&flat, touch_part_symbol(part));

	f_cons(&flat);
	f_sym(&flat, pressed ? SYM_TRUE : SYM_NIL);

	f_cons(&flat);
	f_u(&flat, pressed ? point->x : 0);

	f_cons(&flat);
	f_u(&flat, pressed ? point->y : 0);

	f_cons(&flat);
	f_u(&flat, pressed ? point->strength : 0);

	f_cons(&flat);
	f_u(&flat, pressed ? point->track_id : 0);

	f_sym(&flat, SYM_NIL);
	lbm_finish_flatten(&flat);

	if (!lbm_event(&flat)) {
		lbm_free(flat.buf);
	}
}

static bool touch_validate_i2c_load_args(int pin_sda, int pin_scl, int pin_rst, int pin_int, int width, int height, int freq) {
	if (!utils_gpio_is_valid(pin_sda) ||
			!utils_gpio_is_valid(pin_scl) ||
			!touch_core_gpio_valid_or_nc(pin_rst) ||
			!touch_core_gpio_valid_or_nc(pin_int)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return false;
	}

	if (width <= 0 || height <= 0) {
		lbm_set_error_reason(msg_invalid_size);
		return false;
	}

	if (freq <= 0) {
		lbm_set_error_reason(msg_invalid_i2c_speed);
		return false;
	}

	return true;
}

// Every i2c load takes the same six or seven arguments and differs only in
// which part it asks the core for, so they share one body.
static lbm_value touch_load_i2c_part(touch_part_t part, lbm_value *args, lbm_uint argn) {
	int pin_sda = lbm_dec_as_i32(args[0]);
	int pin_scl = lbm_dec_as_i32(args[1]);
	int pin_rst = lbm_dec_as_i32(args[2]);
	int pin_int = lbm_dec_as_i32(args[3]);
	int width = lbm_dec_as_i32(args[4]);
	int height = lbm_dec_as_i32(args[5]);
	int freq = TOUCH_I2C_DEFAULT_FREQ;

	if (argn == 7) {
		freq = lbm_dec_as_i32(args[6]);
	}

	if (!touch_validate_i2c_load_args(pin_sda, pin_scl, pin_rst, pin_int, width, height, freq)) {
		return ENC_SYM_EERROR;
	}

	if (!touch_core_init()) {
		lbm_set_error_reason(msg_touch_runtime);
		return ENC_SYM_EERROR;
	}

	esp_err_t res = touch_core_load_i2c(part, pin_sda, pin_scl, pin_rst, pin_int,
			(uint16_t)width, (uint16_t)height, (uint32_t)freq);
	if (res != ESP_OK) {
		lbm_set_esp_error_reason(res);
		return ENC_SYM_EERROR;
	}

	return ENC_SYM_TRUE;
}

static lbm_value ext_touch_load_cst816s(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_RANGE(6, 7);
	return touch_load_i2c_part(TOUCH_PART_CST816S, args, argn);
}

static lbm_value ext_touch_load_gt911(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_RANGE(6, 7);
	return touch_load_i2c_part(TOUCH_PART_GT911, args, argn);
}

static lbm_value ext_touch_load_cst9217(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_RANGE(6, 7);
	return touch_load_i2c_part(TOUCH_PART_CST9217, args, argn);
}

static lbm_value ext_touch_load_axs15231(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_RANGE(6, 7);
	return touch_load_i2c_part(TOUCH_PART_AXS15231, args, argn);
}

static lbm_value ext_touch_load_cst836u(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_RANGE(6, 7);
	return touch_load_i2c_part(TOUCH_PART_CST836U, args, argn);
}

static lbm_value ext_touch_load_xpt2046(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_RANGE(8, 9);

	int host = lbm_dec_as_i32(args[0]);
	int pin_mosi = lbm_dec_as_i32(args[1]);
	int pin_miso = lbm_dec_as_i32(args[2]);
	int pin_sclk = lbm_dec_as_i32(args[3]);
	int pin_cs = lbm_dec_as_i32(args[4]);
	int pin_int = lbm_dec_as_i32(args[5]);
	int width = lbm_dec_as_i32(args[6]);
	int height = lbm_dec_as_i32(args[7]);
	int freq = 2500000;

	if (argn == 9) {
		freq = lbm_dec_as_i32(args[8]);
	}

	if (host < SPI2_HOST || host > SPI3_HOST) {
		lbm_set_error_reason(msg_invalid_spi_host);
		return ENC_SYM_EERROR;
	}

	if (!utils_gpio_is_valid(pin_mosi) ||
			!utils_gpio_is_valid(pin_miso) ||
			!utils_gpio_is_valid(pin_sclk) ||
			!utils_gpio_is_valid(pin_cs) ||
			!touch_core_gpio_valid_or_nc(pin_int)) {
		lbm_set_error_reason(msg_invalid_gpio);
		return ENC_SYM_EERROR;
	}

	if (width <= 0 || height <= 0) {
		lbm_set_error_reason(msg_invalid_size);
		return ENC_SYM_EERROR;
	}

	if (freq <= 0) {
		lbm_set_error_reason(msg_invalid_i2c_speed);
		return ENC_SYM_EERROR;
	}

	if (!touch_core_init()) {
		lbm_set_error_reason(msg_touch_runtime);
		return ENC_SYM_EERROR;
	}

	esp_err_t res = touch_core_load_spi(TOUCH_PART_XPT2046, host, pin_mosi,
			pin_miso, pin_sclk, pin_cs, pin_int, (uint16_t)width,
			(uint16_t)height, (uint32_t)freq);
	if (res != ESP_OK) {
		lbm_set_esp_error_reason(res);
		return ENC_SYM_EERROR;
	}

	return ENC_SYM_TRUE;
}

static lbm_value ext_touch_read(lbm_value *args, lbm_uint argn) {
	(void)args;
	LBM_CHECK_ARGN(0);

	touch_point_t point;
	uint8_t point_cnt = 0;
	esp_err_t res = touch_core_read(&point, &point_cnt, 1);

	if (res == ESP_ERR_INVALID_STATE) {
		lbm_set_error_reason(msg_touch_not_loaded);
		return ENC_SYM_EERROR;
	}

	if (res != ESP_OK) {
		lbm_set_esp_error_reason(res);
		return ENC_SYM_EERROR;
	}

	if (point_cnt == 0) {
		return ENC_SYM_NIL;
	}

	return make_list(
			4,
			lbm_enc_u(point.x),
			lbm_enc_u(point.y),
			lbm_enc_u(point.strength),
			lbm_enc_u(point.track_id));
}

/*
 * (touch-stats) -> (ok err last-err)
 *
 * touch-read reports a failed read as nil, the same as an untouched panel, so
 * a script can poll unconditionally on a board whose touch did not come up.
 * That makes a controller which has stopped answering look exactly like a
 * finger that is not there, and on a board whose only input is touch that is
 * the first thing worth knowing.
 */
static lbm_value ext_touch_stats(lbm_value *args, lbm_uint argn) {
	(void)args; (void)argn;

	uint32_t ok = 0, err = 0;
	int last = 0;
	touch_core_read_stats(&ok, &err, &last);

	lbm_value res = ENC_SYM_NIL;
	// Built back to front, since cons prepends.
	res = lbm_cons(lbm_enc_i(last), res);
	res = lbm_cons(lbm_enc_u32(err), res);
	res = lbm_cons(lbm_enc_u32(ok), res);
	return res;
}

static lbm_value ext_touch_read_all(lbm_value *args, lbm_uint argn) {
	(void)args;
	LBM_CHECK_ARGN(0);

	touch_point_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
	uint8_t point_cnt = 0;
	esp_err_t res = touch_core_read(points, &point_cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

	if (res == ESP_ERR_INVALID_STATE) {
		lbm_set_error_reason(msg_touch_not_loaded);
		return ENC_SYM_EERROR;
	}

	if (res != ESP_OK) {
		lbm_set_esp_error_reason(res);
		return ENC_SYM_EERROR;
	}

	if (point_cnt == 0) {
		return ENC_SYM_NIL;
	}

	// Built after the core has released its lock, so a long list does not
	// hold up event delivery.
	lbm_value result = ENC_SYM_NIL;
	for (int i = (int)point_cnt - 1; i >= 0; i--) {
		lbm_value point = ENC_SYM_NIL;
		point = lbm_cons(lbm_enc_u(points[i].track_id), point);
		if (point == ENC_SYM_MERROR) {
			return ENC_SYM_MERROR;
		}
		point = lbm_cons(lbm_enc_u(points[i].strength), point);
		if (point == ENC_SYM_MERROR) {
			return ENC_SYM_MERROR;
		}
		point = lbm_cons(lbm_enc_u(points[i].y), point);
		if (point == ENC_SYM_MERROR) {
			return ENC_SYM_MERROR;
		}
		point = lbm_cons(lbm_enc_u(points[i].x), point);
		if (point == ENC_SYM_MERROR) {
			return ENC_SYM_MERROR;
		}

		result = lbm_cons(point, result);
		if (result == ENC_SYM_MERROR) {
			return ENC_SYM_MERROR;
		}
	}

	return result;
}

static lbm_value ext_touch_delete(lbm_value *args, lbm_uint argn) {
	(void)args;
	LBM_CHECK_ARGN(0);

	touch_core_delete();
	return ENC_SYM_TRUE;
}

static lbm_value ext_touch_apply_transforms(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(3);

	bool swap_xy = lbm_dec_as_i32(args[0]) != 0;
	bool mirror_x = lbm_dec_as_i32(args[1]) != 0;
	bool mirror_y = lbm_dec_as_i32(args[2]) != 0;

	esp_err_t res = touch_core_set_transforms(swap_xy, mirror_x, mirror_y);

	if (res == ESP_ERR_INVALID_STATE) {
		lbm_set_error_reason(msg_touch_not_loaded);
		return ENC_SYM_EERROR;
	}

	if (res != ESP_OK) {
		lbm_set_esp_error_reason(res);
		return ENC_SYM_EERROR;
	}

	return ENC_SYM_TRUE;
}

void lispif_load_touch_extensions(void) {
	lbm_add_symbol_const("cst816s", &sym_cst816s);
	lbm_add_symbol_const("gt911", &sym_gt911);
	lbm_add_symbol_const("cst9217", &sym_cst9217);
	lbm_add_symbol_const("xpt2046", &sym_xpt2046);
	lbm_add_symbol_const("axs15231", &sym_axs15231);
	lbm_add_symbol_const("cst836u", &sym_cst836u);

	touch_core_set_event_cb(touch_emit_event, touch_events_enabled);

	lbm_add_extension("touch-load-cst816s", ext_touch_load_cst816s);
	lbm_add_extension("touch-load-gt911", ext_touch_load_gt911);
	lbm_add_extension("touch-load-cst9217", ext_touch_load_cst9217);
	lbm_add_extension("touch-load-xpt2046", ext_touch_load_xpt2046);
	lbm_add_extension("touch-load-axs15231", ext_touch_load_axs15231);
	lbm_add_extension("touch-load-cst836u", ext_touch_load_cst836u);
	lbm_add_extension("touch-read", ext_touch_read);
	lbm_add_extension("touch-read-all", ext_touch_read_all);
	lbm_add_extension("touch-stats", ext_touch_stats);
	lbm_add_extension("touch-delete", ext_touch_delete);
	lbm_add_extension("touch-apply-transforms", ext_touch_apply_transforms);
}
