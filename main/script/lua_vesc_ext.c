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

/*
 * VESC bindings for the Lua engine.
 *
 * Naming rule, applied without exception so the two engines' surfaces can be
 * compared mechanically: the lisp extension `can-send-sid` becomes
 * `vesc.can_send_sid`. Lua has no hyphen in identifiers, and a table is a
 * better fit than 200 globals. tools/script_ext_coverage.py diffs the two
 * surfaces using exactly that rule, so an extension ported here shows up as
 * covered with no list to maintain by hand.
 *
 * This is a first set, not the full surface. Everything here is bound against
 * an API that was read in the source rather than assumed, and the coverage
 * tool reports honestly on what is still missing.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "commands.h"
#include "comm_can.h"
#include "comm_usb.h"
#include "comm_wifi.h"
#include "mempools.h"
#include "nmea.h"
#include "esp_system.h"
#include "flash_helper.h"
#include "adc.h"
#include "utils.h"
#include "datatypes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include <string.h>
#include "log_ring.h"
#include "esp_timer.h"

// ---------------------------------------------------------------- system ---

/*
 * vesc.log_lines([max]) -> {string, ...}, dropped
 *
 * The most recent firmware log lines, oldest first, which is reading order
 * for a boot log. Everything commands_printf, the ESP-IDF log and a script's
 * own print produced, including the lines from before anything connected to
 * the board -- those are sent to whichever port last spoke, which during
 * bring-up is none, so they would otherwise be gone.
 *
 * The second return is how many lines were dropped for want of room. A log
 * that silently loses its beginning is worse than one that says it did, and a
 * display showing the log has somewhere to say it.
 */
static int l_log_lines(lua_State *L) {
	lua_Integer want = luaL_optinteger(L, 1, LOG_RING_LINES);
	if (want < 1) {
		want = 1;
	}
	if (want > LOG_RING_LINES) {
		want = LOG_RING_LINES;
	}

	// On the stack rather than the heap: the script engine's allocator has a
	// ceiling and this is a diagnostic path, which is the worst place to need
	// an allocation to succeed.
	static char lines[LOG_RING_LINES][LOG_RING_LINE_LEN];
	int n = log_ring_read(lines, (int)want);

	lua_createtable(L, n, 0);
	for (int i = 0; i < n; i++) {
		lua_pushstring(L, lines[i]);
		lua_rawseti(L, -2, i + 1);
	}

	lua_pushinteger(L, (lua_Integer)log_ring_dropped());
	return 2;
}

// vesc.log_add(text) -- a line of the script's own, without printing it.
static int l_log_add(lua_State *L) {
	log_ring_add(luaL_checkstring(L, 1));
	return 0;
}

/*
 * vesc.micros() -> microseconds since boot, as an integer.
 *
 * systime is FreeRTOS ticks, which is milliseconds here -- too coarse to time
 * one pass of anything that matters. esp_timer_get_time is 64-bit
 * microseconds; this truncates to the Lua integer, which is int32 in this
 * build, so it wraps every 35 minutes. Fine for a difference, wrong for a
 * timestamp, and the name says which.
 */
static int l_micros(lua_State *L) {
	lua_pushinteger(L, (lua_Integer)(uint32_t)esp_timer_get_time());
	return 1;
}

static int l_systime(lua_State *L) {
	lua_pushinteger(L, (lua_Integer)xTaskGetTickCount());
	return 1;
}

static int l_secs_since(lua_State *L) {
	lua_Integer then = luaL_checkinteger(L, 1);
	TickType_t now = xTaskGetTickCount();
	lua_pushnumber(L, (lua_Number)((TickType_t)now - (TickType_t)then) /
			(lua_Number)configTICK_RATE_HZ);
	return 1;
}

/*
 * Yielding sleep. A script that wants to wait must not spin: the engine task
 * shares a core with the comms stack, and a busy wait here shows up as
 * dropped packets rather than as a slow script.
 */
static int l_sleep(lua_State *L) {
	lua_Number secs = luaL_checknumber(L, 1);
	if (secs < 0) {
		secs = 0;
	}
	// Round up, so sleep(0.0001) yields rather than becoming a no-op.
	TickType_t ticks = (TickType_t)((secs * (lua_Number)configTICK_RATE_HZ) + 0.999);
	vTaskDelay(ticks);
	return 0;
}

// ------------------------------------------------------------------ gpio ---

static int l_gpio_configure(lua_State *L) {
	int pin = (int)luaL_checkinteger(L, 1);
	const char *mode = luaL_optstring(L, 2, "in");

	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << (unsigned)pin,
		.intr_type = GPIO_INTR_DISABLE,
	};

	if (strcmp(mode, "out") == 0) {
		cfg.mode = GPIO_MODE_OUTPUT;
	} else if (strcmp(mode, "in-pu") == 0) {
		cfg.mode = GPIO_MODE_INPUT;
		cfg.pull_up_en = GPIO_PULLUP_ENABLE;
	} else if (strcmp(mode, "in-pd") == 0) {
		cfg.mode = GPIO_MODE_INPUT;
		cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
	} else if (strcmp(mode, "in") == 0) {
		cfg.mode = GPIO_MODE_INPUT;
	} else {
		return luaL_error(L, "gpio_configure: unknown mode '%s' "
				"(expected in, in-pu, in-pd or out)", mode);
	}

	lua_pushboolean(L, gpio_config(&cfg) == ESP_OK);
	return 1;
}

static int l_gpio_write(lua_State *L) {
	int pin = (int)luaL_checkinteger(L, 1);
	int level = lua_toboolean(L, 2);
	lua_pushboolean(L, gpio_set_level((gpio_num_t)pin, (uint32_t)level) == ESP_OK);
	return 1;
}

static int l_gpio_read(lua_State *L) {
	int pin = (int)luaL_checkinteger(L, 1);
	lua_pushboolean(L, gpio_get_level((gpio_num_t)pin));
	return 1;
}

// ------------------------------------------------------------------- can ---

/*
 * CAN payloads are Lua strings rather than tables of bytes: a string is
 * already a counted byte buffer, string.pack builds one in the format the
 * other end wants, and it avoids allocating a table per frame in a loop that
 * may run at a few hundred hertz.
 */
static const uint8_t *check_can_payload(lua_State *L, int idx, uint8_t *len_out) {
	size_t len = 0;
	const char *data = luaL_checklstring(L, idx, &len);
	if (len > 8) {
		luaL_error(L, "a CAN frame carries at most 8 bytes, got %d", (int)len);
		return NULL;
	}
	*len_out = (uint8_t)len;
	return (const uint8_t *)data;
}

static int l_can_send_sid(lua_State *L) {
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	uint8_t len = 0;
	const uint8_t *data = check_can_payload(L, 2, &len);
	comm_can_transmit_sid(id, data, len);
	return 0;
}

static int l_can_send_eid(lua_State *L) {
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	uint8_t len = 0;
	const uint8_t *data = check_can_payload(L, 2, &len);
	comm_can_transmit_eid(id, data, len);
	return 0;
}

static int l_can_ping(lua_State *L) {
	int id = (int)luaL_checkinteger(L, 1);
	if (id < 0 || id > 254) {
		return luaL_error(L, "can_ping: id %d is out of range", id);
	}
	HW_TYPE hw = HW_TYPE_VESC;
	bool ok = comm_can_ping((uint8_t)id, &hw);
	lua_pushboolean(L, ok);
	lua_pushinteger(L, (lua_Integer)hw);
	return 2;
}

/*
 * Status getters for other units on the bus.
 *
 * Every one returns nil when that id has not reported, rather than zero. A
 * controller that is switched off, unplugged or not yet seen is a different
 * thing from one reporting zero current, and collapsing the two is how a
 * dash ends up displaying a confident 0 A for a motor that is not there.
 * Field mappings were read out of the lisp implementations rather than
 * guessed, so the two engines report the same numbers.
 */
static int l_canget_current(lua_State *L) {
	can_status_msg *st = comm_can_get_status_msg_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->current);
	return 1;
}

static int l_canget_current_dir(lua_State *L) {
	can_status_msg *st = comm_can_get_status_msg_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	// Signed by direction of travel, which is what the lisp getter does.
	float sign = st->duty >= 0.0f ? 1.0f : -1.0f;
	lua_pushnumber(L, (lua_Number)(st->current * sign));
	return 1;
}

static int l_canget_duty(lua_State *L) {
	can_status_msg *st = comm_can_get_status_msg_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->duty);
	return 1;
}

static int l_canget_rpm(lua_State *L) {
	can_status_msg *st = comm_can_get_status_msg_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->rpm);
	return 1;
}

/*
 * vesc.canget_speed(id)
 *
 * Carries an upstream quirk deliberately: the lisp canget-speed on this
 * firmware returns the status frame's rpm, not a speed -- there is no gearing
 * here to turn one into the other. Bound under the same name so a ported
 * script behaves identically, and documented so the name does not mislead.
 */
static int l_canget_speed(lua_State *L) {
	can_status_msg *st = comm_can_get_status_msg_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->rpm);
	return 1;
}

static int l_canget_current_in(lua_State *L) {
	can_status_msg_4 *st = comm_can_get_status_msg_4_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->current_in);
	return 1;
}

static int l_canget_temp_fet(lua_State *L) {
	can_status_msg_4 *st = comm_can_get_status_msg_4_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->temp_fet);
	return 1;
}

static int l_canget_temp_motor(lua_State *L) {
	can_status_msg_4 *st = comm_can_get_status_msg_4_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->temp_motor);
	return 1;
}

static int l_canget_vin(lua_State *L) {
	can_status_msg_5 *st = comm_can_get_status_msg_5_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->v_in);
	return 1;
}

static int l_canget_tacho(lua_State *L) {
	can_status_msg_5 *st = comm_can_get_status_msg_5_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, (lua_Integer)st->tacho_value);
	return 1;
}

static int l_canget_ppm(lua_State *L) {
	can_status_msg_6 *st = comm_can_get_status_msg_6_id((int)luaL_checkinteger(L, 1));
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)st->ppm);
	return 1;
}

static int l_canget_adc(lua_State *L) {
	can_status_msg_6 *st = comm_can_get_status_msg_6_id((int)luaL_checkinteger(L, 1));
	int ch = (int)luaL_optinteger(L, 2, 1);
	if (!st) {
		lua_pushnil(L);
		return 1;
	}
	switch (ch) {
	case 1: lua_pushnumber(L, (lua_Number)st->adc_1); break;
	case 2: lua_pushnumber(L, (lua_Number)st->adc_2); break;
	case 3: lua_pushnumber(L, (lua_Number)st->adc_3); break;
	default:
		return luaL_error(L, "canget_adc: channel %d is not 1, 2 or 3", ch);
	}
	return 1;
}

/*
 * Age of the last status frame of a given kind, in seconds, or nil if none
 * has arrived.
 *
 * This is the one to check before trusting any of the getters above: the
 * others hand back the last value seen with no indication of when that was,
 * so a unit that dropped off the bus a minute ago still reads plausibly.
 */
static int l_can_msg_age(lua_State *L) {
	int id = (int)luaL_checkinteger(L, 1);
	int msg = (int)luaL_optinteger(L, 2, 1);

	float rx_time = -1.0f;
	switch (msg) {
	case 1: {
		can_status_msg *st = comm_can_get_status_msg_id(id);
		if (st) { rx_time = UTILS_AGE_S(st->rx_time); }
	} break;
	case 2: {
		can_status_msg_2 *st = comm_can_get_status_msg_2_id(id);
		if (st) { rx_time = UTILS_AGE_S(st->rx_time); }
	} break;
	case 3: {
		can_status_msg_3 *st = comm_can_get_status_msg_3_id(id);
		if (st) { rx_time = UTILS_AGE_S(st->rx_time); }
	} break;
	case 4: {
		can_status_msg_4 *st = comm_can_get_status_msg_4_id(id);
		if (st) { rx_time = UTILS_AGE_S(st->rx_time); }
	} break;
	case 5: {
		can_status_msg_5 *st = comm_can_get_status_msg_5_id(id);
		if (st) { rx_time = UTILS_AGE_S(st->rx_time); }
	} break;
	case 6: {
		can_status_msg_6 *st = comm_can_get_status_msg_6_id(id);
		if (st) { rx_time = UTILS_AGE_S(st->rx_time); }
	} break;
	default:
		return luaL_error(L, "can_msg_age: status message %d is not 1..6", msg);
	}

	if (rx_time < 0.0f) {
		lua_pushnil(L);
	} else {
		lua_pushnumber(L, (lua_Number)rx_time);
	}
	return 1;
}

// Units seen on the bus, as a table of ids.
static int l_can_list_devs(lua_State *L) {
	lua_newtable(L);
	int n = 0;
	for (int i = 0; i < CAN_STATUS_MSGS_TO_STORE; i++) {
		can_status_msg *msg = comm_can_get_status_msg_index(i);
		if (!msg || msg->id < 0 || UTILS_AGE_S(msg->rx_time) >= 2.0) {
			continue;
		}
		lua_pushinteger(L, (lua_Integer)msg->id);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

// ------------------------------------------------------------ can control --

/*
 * Commands to other units.
 *
 * The id is checked before every one of these. comm_can_set_* takes a
 * uint8_t, so a negative or oversized id would wrap silently and address a
 * unit the script did not mean -- on a vehicle that is a command going to the
 * wrong motor.
 */
static uint8_t check_can_id(lua_State *L, int idx) {
	lua_Integer id = luaL_checkinteger(L, idx);
	if (id < 0 || id > 253) {
		luaL_error(L, "CAN id %d is outside 0..253", (int)id);
	}
	return (uint8_t)id;
}

static int l_canset_current(lua_State *L) {
	uint8_t id = check_can_id(L, 1);
	float cur = (float)luaL_checknumber(L, 2);
	if (lua_gettop(L) >= 3) {
		comm_can_set_current_off_delay(id, cur, (float)luaL_checknumber(L, 3));
	} else {
		comm_can_set_current(id, cur);
	}
	return 0;
}

static int l_canset_current_rel(lua_State *L) {
	uint8_t id = check_can_id(L, 1);
	float rel = (float)luaL_checknumber(L, 2);
	if (lua_gettop(L) >= 3) {
		comm_can_set_current_rel_off_delay(id, rel, (float)luaL_checknumber(L, 3));
	} else {
		comm_can_set_current_rel(id, rel);
	}
	return 0;
}

static int l_canset_duty(lua_State *L) {
	comm_can_set_duty(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

static int l_canset_brake(lua_State *L) {
	comm_can_set_current_brake(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

static int l_canset_brake_rel(lua_State *L) {
	comm_can_set_current_brake_rel(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

static int l_canset_rpm(lua_State *L) {
	comm_can_set_rpm(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

static int l_canset_pos(lua_State *L) {
	comm_can_set_pos(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

static int l_canset_handbrake(lua_State *L) {
	comm_can_set_handbrake(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

static int l_canset_handbrake_rel(lua_State *L) {
	comm_can_set_handbrake_rel(check_can_id(L, 1), (float)luaL_checknumber(L, 2));
	return 0;
}

// ---------------------------------------------------------------- eeprom ---

/*
 * The same 512-slot store the lisp engine exposes, and deliberately the same
 * slot numbering: a package ported from lisp to Lua keeps its stored settings
 * rather than silently starting from defaults.
 */
static int check_eeprom_addr(lua_State *L, int idx) {
	lua_Integer addr = luaL_checkinteger(L, idx);
	if (addr < 0 || addr >= EEPROM_VARS) {
		luaL_error(L, "eeprom address %d is outside 0..%d",
				(int)addr, EEPROM_VARS - 1);
	}
	return (int)addr;
}

static int l_eeprom_store_i(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	v.as_i32 = (int32_t)luaL_checkinteger(L, 2);
	lua_pushboolean(L, store_eeprom_var(&v, addr, 1));
	return 1;
}

static int l_eeprom_store_f(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	v.as_float = (float)luaL_checknumber(L, 2);
	lua_pushboolean(L, store_eeprom_var(&v, addr, 1));
	return 1;
}

/*
 * Returns nil for an unwritten slot rather than zero.
 *
 * This is the one place the lisp engine's behaviour is worth repeating
 * exactly, and it is a trap: on hardware an unwritten slot reads as nothing,
 * while a host test stub tends to hand back zero. A dash package that treated
 * the zero as a real setting worked in tests and fell over on a board.
 */
static int l_eeprom_read_i(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	if (!read_eeprom_var(&v, addr, 1)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, (lua_Integer)v.as_i32);
	return 1;
}

static int l_eeprom_read_f(lua_State *L) {
	int addr = check_eeprom_addr(L, 1);
	eeprom_var v;
	if (!read_eeprom_var(&v, addr, 1)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, (lua_Number)v.as_float);
	return 1;
}

static int l_eeprom_erase(lua_State *L) {
	lua_pushboolean(L, erase_eeprom_var(0, EEPROM_VARS));
	return 1;
}

// ------------------------------------------------------------------- adc ---

static int l_get_adc(lua_State *L) {
	int ch = (int)luaL_optinteger(L, 1, 0);
	lua_pushnumber(L, (lua_Number)adc_get_voltage((adc1_channel_t)ch));
	return 1;
}

// ---------------------------------------------------------------- events ---

/*
 * Events the queue had to drop because the script could not keep up.
 *
 * Exposed rather than merely logged because a handler is the only thing that
 * knows whether missing frames matter. A script sampling a sensor can ignore
 * a gap; one counting wheel pulses cannot, and should be able to notice and
 * say so.
 */
uint32_t luaif_events_dropped(void);

static int l_events_dropped(lua_State *L) {
	lua_pushinteger(L, (lua_Integer)luaif_events_dropped());
	return 1;
}

// ------------------------------------------------------------ registration --

/*
 * vesc.reboot() -- does not return.
 *
 * Drops the wifi link first and gives it a moment, as the lisp binding does;
 * restarting with the co-processor mid-transaction leaves it to time out on
 * its own.
 */
static int l_reboot(lua_State *L) {
	(void)L;
	comm_wifi_disconnect();
	vTaskDelay(50 / portTICK_PERIOD_MS);
	esp_restart();
	return 0;
}

// vesc.set_print_prefix("DISP-") -- tags this script's output.
static int l_set_print_prefix(lua_State *L) {
	const char *prefix = luaL_checkstring(L, 1);
	luaif_set_print_prefix(prefix);
	return 0;
}

// vesc.gnss_speed() -> m/s from the last RMC sentence.
static int l_gnss_speed(lua_State *L) {
	lua_pushnumber(L, nmea_get_state()->rmc.speed);
	return 1;
}

/*
 * vesc.send_data(data [, interface [, can_id]])
 *
 * data is a string, which is how Lua carries bytes -- string.pack builds one
 * in whatever layout the receiver expects. Goes out as COMM_CUSTOM_APP_DATA,
 * which is what a companion app or a dash protocol reads.
 *
 * interface: 0 the current comm port, 1 USB, 2 CAN (with can_id), 3 the
 * local wifi socket. Same numbering as the lisp binding.
 */
static int l_send_data(lua_State *L) {
	size_t len = 0;
	const char *data = luaL_checklstring(L, 1, &len);
	int interface = (int)luaL_optinteger(L, 2, 0);
	int can_id = (int)luaL_optinteger(L, 3, 0);

	// One byte for the command id, and the buffer is a fixed mempool block.
	if (len > 400) {
		return luaL_error(L, "send_data: %d bytes is more than 400", (int)len);
	}

	uint8_t *buf = mempools_get_packet_buffer();
	if (!buf) {
		return luaL_error(L, "send_data: no packet buffer free");
	}

	int ind = 0;
	buf[ind++] = COMM_CUSTOM_APP_DATA;
	memcpy(buf + ind, data, len);
	ind += (int)len;

	switch (interface) {
	case 1:
		comm_usb_send_packet(buf, ind);
		break;
	case 2:
		comm_can_send_buffer(can_id, buf, ind, 3);
		break;
	default:
		commands_send_packet(buf, ind);
		break;
	}

	mempools_free_packet_buffer(buf);
	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg vesc_fns[] = {
	{"log_lines", l_log_lines},
	{"log_add", l_log_add},

	{"micros", l_micros},
	{"systime", l_systime},
	{"secs_since", l_secs_since},
	{"sleep", l_sleep},

	{"gpio_configure", l_gpio_configure},
	{"gpio_write", l_gpio_write},
	{"gpio_read", l_gpio_read},

	{"can_send_sid", l_can_send_sid},
	{"can_send_eid", l_can_send_eid},
	{"can_ping", l_can_ping},
	{"can_list_devs", l_can_list_devs},

	{"canget_current", l_canget_current},
	{"canget_current_dir", l_canget_current_dir},
	{"canget_current_in", l_canget_current_in},
	{"canget_duty", l_canget_duty},
	{"canget_rpm", l_canget_rpm},
	{"canget_speed", l_canget_speed},
	{"canget_temp_fet", l_canget_temp_fet},
	{"canget_temp_motor", l_canget_temp_motor},
	{"canget_vin", l_canget_vin},
	{"canget_ppm", l_canget_ppm},
	{"canget_adc", l_canget_adc},
	{"canget_tacho", l_canget_tacho},
	{"canget_dist", l_canget_tacho},
	{"can_msg_age", l_can_msg_age},

	{"canset_current", l_canset_current},
	{"canset_current_rel", l_canset_current_rel},
	{"canset_duty", l_canset_duty},
	{"canset_brake", l_canset_brake},
	{"canset_brake_rel", l_canset_brake_rel},
	{"canset_rpm", l_canset_rpm},
	{"canset_pos", l_canset_pos},
	{"canset_handbrake", l_canset_handbrake},
	{"canset_handbrake_rel", l_canset_handbrake_rel},

	{"eeprom_store_i", l_eeprom_store_i},
	{"eeprom_store_f", l_eeprom_store_f},
	{"eeprom_read_i", l_eeprom_read_i},
	{"eeprom_read_f", l_eeprom_read_f},
	{"eeprom_erase", l_eeprom_erase},

	{"get_adc", l_get_adc},

	{"events_dropped", l_events_dropped},
	{"reboot", l_reboot},
	{"set_print_prefix", l_set_print_prefix},
	{"send_data", l_send_data},
	{"gnss_speed", l_gnss_speed},

	{NULL, NULL},
};

void lua_vesc_ext_register(script_lua_t *s) {
	script_lua_register(s, vesc_fns);
}

const char **lua_vesc_ext_names(int *count) {
	static const char *names[sizeof(vesc_fns) / sizeof(vesc_fns[0])];
	int n = 0;
	for (const luaL_Reg *f = vesc_fns; f->name; f++) {
		names[n++] = f->name;
	}
	if (count) {
		*count = n;
	}
	return names;
}
