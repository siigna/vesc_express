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
 * Touch controller core, with no script engine in it.
 *
 * This is the esp_lcd_touch plumbing that used to be interleaved with the
 * LispBM bindings in lispif_touch_extensions.c: bus setup for the six
 * supported parts, the shared handle and its mutex, the poll/interrupt task,
 * and the transform application. Both engines sit on top of it as bindings
 * that validate arguments and convert values, and neither owns the hardware.
 *
 * The split exists because the Lua engine previously carried its own
 * hundred-line I2C touch path. That was a deliberate stopgap -- the file said
 * so -- and it cost Lua interrupt events, multi-touch and four of the six
 * controllers.
 *
 * Two things a caller has to respect:
 *
 *   - touch_core_lock() must be paired with touch_core_unlock(). The event
 *     task takes the same mutex, so holding it across anything slow stalls
 *     event delivery.
 *
 *   - The event callback runs on the core's own task, not the caller's. It
 *     must not block, and on the lisp side it must not touch the interpreter
 *     except through the event queue.
 */

#ifndef TOUCH_CORE_H_
#define TOUCH_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

typedef struct {
	uint8_t track_id;
	uint16_t x;
	uint16_t y;
	uint16_t strength;
} touch_point_t;

typedef struct {
	esp_err_t (*deinit)(void);
	esp_err_t (*read_data)(void);
	esp_err_t (*get_data)(touch_point_t *data, uint8_t *point_cnt, uint8_t max_point_cnt);
} touch_driver_t;

typedef enum {
	TOUCH_PART_NONE = 0,
	TOUCH_PART_CST816S,
	TOUCH_PART_GT911,
	TOUCH_PART_CST9217,
	TOUCH_PART_XPT2046,
	TOUCH_PART_AXS15231,
	TOUCH_PART_CST836U,
} touch_part_t;

// Called from the core's event task when the contact state changes or the
// point has moved far enough. point is valid only when pressed.
typedef void (*touch_core_event_cb_t)(bool pressed, const touch_point_t *point,
		touch_part_t part);

// Mutex and event task. Idempotent, and must succeed before a load.
bool touch_core_init(void);

/*
 * Bring up one of the I2C parts, or the SPI one. Pins are as wired; -1 means
 * not connected for rst and int. On success the part becomes the loaded
 * driver and the previous one is released.
 *
 * Validation of pin numbers and sizes stays with the caller, which owns the
 * error messages its engine reports.
 */
esp_err_t touch_core_load_i2c(touch_part_t part, int sda, int scl, int rst,
		int int_pin, uint16_t width, uint16_t height, uint32_t freq);
esp_err_t touch_core_load_spi(touch_part_t part, int host, int mosi, int miso,
		int sclk, int cs, int int_pin, uint16_t width, uint16_t height,
		uint32_t freq);

bool touch_core_loaded(void);
touch_part_t touch_core_part(void);

// Lock, failing if nothing is loaded. Only for a caller that needs the handle
// held across several operations; touch_core_read does its own locking.
bool touch_core_lock(void);
void touch_core_unlock(void);

// Read the controller and return up to max points. Locks internally.
/*
 * How many reads succeeded, how many failed, and the last error.
 *
 * The read path reports a failure as "not touched" so a script can poll
 * unconditionally, which means a controller that has fallen off the bus looks
 * exactly like a finger that is not there. These are how a script tells the
 * difference. Any pointer may be NULL.
 */
void touch_core_read_stats(uint32_t *ok, uint32_t *err, int *last_err);

esp_err_t touch_core_read(touch_point_t *points, uint8_t *point_cnt,
		uint8_t max_point_cnt);

esp_err_t touch_core_set_transforms(bool swap_xy, bool mirror_x, bool mirror_y);

void touch_core_delete(void);

// Event delivery. enabled is consulted on every poll so an engine can gate
// events without tearing the callback down; passing NULL for it means always.
void touch_core_set_event_cb(touch_core_event_cb_t cb, bool (*enabled)(void));

// From the esp_lcd_touch interrupt. Safe to call with no task running.
void touch_core_irq_from_isr(void);

// true for a pin that is either valid or deliberately not connected.
bool touch_core_gpio_valid_or_nc(int pin);

#endif
