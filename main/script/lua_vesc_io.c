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

/*
 * UART and I2C bindings for the Lua engine.
 *
 * Separate from lua_vesc_ext.c because both need driver state of their own,
 * and because both are shaped by one decision worth stating: byte strings
 * rather than tables.
 *
 * LispBM passes byte buffers as lists or arrays, so its uart-write takes a
 * list of numbers. In Lua a string already is a counted byte buffer, and
 * string.pack and string.unpack build and read whatever layout a device
 * wants, which is exactly the job. It is also far cheaper: a 32-byte frame is
 * one string rather than 32 boxed numbers in a table.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "driver/uart.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "utils.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

// ------------------------------------------------------------------ uart ---

// Matches the lisp engine's buffer size, so a script ported between engines
// sees the same behaviour when a peer sends faster than it reads.
#define UART_RX_BUF		512
#define UART_MAX_WRITE		512

static int m_uart_num = -1;

static int l_uart_start(lua_State *L) {
	int num = (int)luaL_checkinteger(L, 1);
	int rx = (int)luaL_checkinteger(L, 2);
	int tx = (int)luaL_checkinteger(L, 3);
	int baud = (int)luaL_checkinteger(L, 4);

	if (baud < 10 || baud > 10000000) {
		return luaL_error(L, "uart_start: baud %d is outside 10..10000000", baud);
	}
	if (num < 0 || num >= UART_NUM_MAX) {
		return luaL_error(L, "uart_start: port %d does not exist", num);
	}

	// Re-starting is allowed and is the normal way to change baud, so an
	// existing driver is torn down rather than refused.
	if (m_uart_num >= 0) {
		uart_driver_delete((uart_port_t)m_uart_num);
		m_uart_num = -1;
	}

	uart_config_t cfg = {
		.baud_rate = baud,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	if (uart_param_config((uart_port_t)num, &cfg) != ESP_OK ||
			uart_set_pin((uart_port_t)num, tx, rx,
					UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
			uart_driver_install((uart_port_t)num, UART_RX_BUF, 0, 0, NULL, 0) != ESP_OK) {
		lua_pushboolean(L, 0);
		return 1;
	}

	m_uart_num = num;
	lua_pushboolean(L, 1);
	return 1;
}

static int l_uart_stop(lua_State *L) {
	if (m_uart_num >= 0) {
		uart_driver_delete((uart_port_t)m_uart_num);
		m_uart_num = -1;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int l_uart_write(lua_State *L) {
	if (m_uart_num < 0) {
		return luaL_error(L, "uart_write: call uart_start first");
	}

	size_t len = 0;
	const char *data = luaL_checklstring(L, 1, &len);
	if (len > UART_MAX_WRITE) {
		return luaL_error(L, "uart_write: %d bytes exceeds the %d byte limit",
				(int)len, UART_MAX_WRITE);
	}

	int written = uart_write_bytes((uart_port_t)m_uart_num, data, len);
	lua_pushinteger(L, (lua_Integer)written);
	return 1;
}

/*
 * vesc.uart_read(max_bytes, timeout_ms)
 *
 * Returns what arrived as a string, which is empty if nothing did. An empty
 * string rather than nil because a read timing out is the normal case for a
 * polling protocol, not an error worth branching on.
 */
static int l_uart_read(lua_State *L) {
	if (m_uart_num < 0) {
		return luaL_error(L, "uart_read: call uart_start first");
	}

	lua_Integer want = luaL_optinteger(L, 1, 64);
	lua_Integer timeout = luaL_optinteger(L, 2, 0);
	if (want < 1) {
		want = 1;
	}
	if (want > UART_RX_BUF) {
		want = UART_RX_BUF;
	}

	uint8_t buf[UART_RX_BUF];
	int got = uart_read_bytes((uart_port_t)m_uart_num, buf, (uint32_t)want,
			pdMS_TO_TICKS((uint32_t)(timeout < 0 ? 0 : timeout)));
	if (got < 0) {
		got = 0;
	}

	lua_pushlstring(L, (const char *)buf, (size_t)got);
	return 1;
}

// Bytes waiting, so a script can avoid blocking at all.
static int l_uart_available(lua_State *L) {
	if (m_uart_num < 0) {
		lua_pushinteger(L, 0);
		return 1;
	}
	size_t n = 0;
	if (uart_get_buffered_data_len((uart_port_t)m_uart_num, &n) != ESP_OK) {
		n = 0;
	}
	lua_pushinteger(L, (lua_Integer)n);
	return 1;
}

// ------------------------------------------------------------------- i2c ---

/*
 * Port 0, the same one the lisp engine uses, deliberately. A board's I2C
 * devices are wired to one bus and its drivers -- touch controllers, the
 * CH32 helper on some Waveshare boards -- already claim that port, so using
 * a different one here would not give a script a second bus, it would give it
 * a conflict with the board's own hardware.
 */
#define LUA_I2C_PORT		0
#define I2C_TIMEOUT_MS		20

static bool m_i2c_started = false;

static int l_i2c_start(lua_State *L) {
	int sda = (int)luaL_checkinteger(L, 1);
	int scl = (int)luaL_checkinteger(L, 2);
	lua_Integer hz = luaL_optinteger(L, 3, 200000);

	if (hz < 1000 || hz > 1000000) {
		return luaL_error(L, "i2c_start: %d Hz is outside 1000..1000000",
				(int)hz);
	}

	if (m_i2c_started) {
		i2c_driver_delete(LUA_I2C_PORT);
		m_i2c_started = false;
	}

	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = sda,
		.scl_io_num = scl,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = (uint32_t)hz,
	};

	if (i2c_param_config(LUA_I2C_PORT, &conf) != ESP_OK ||
			i2c_driver_install(LUA_I2C_PORT, conf.mode, 0, 0, 0) != ESP_OK) {
		lua_pushboolean(L, 0);
		return 1;
	}

	m_i2c_started = true;
	lua_pushboolean(L, 1);
	return 1;
}

static int l_i2c_stop(lua_State *L) {
	if (m_i2c_started) {
		i2c_driver_delete(LUA_I2C_PORT);
		m_i2c_started = false;
	}
	lua_pushboolean(L, 1);
	return 1;
}

/*
 * vesc.i2c_tx_rx(addr, tx_string, rx_len)
 *
 * Both tx and rx are optional, which covers the three shapes a device needs:
 * write only, read only, and a write followed by a read in one transaction
 * with no stop in between -- the last being how almost every register read
 * works, and the reason this is one call rather than two.
 *
 * Returns the received bytes as a string, or true for a write with no read,
 * and nil on a bus error so a missing device is distinguishable from a device
 * that answered with zeros.
 */
static int l_i2c_tx_rx(lua_State *L) {
	if (!m_i2c_started) {
		return luaL_error(L, "i2c_tx_rx: call i2c_start first");
	}

	lua_Integer addr = luaL_checkinteger(L, 1);
	if (addr < 0 || addr > 127) {
		return luaL_error(L, "i2c_tx_rx: address %d is outside 0..127",
				(int)addr);
	}

	size_t tx_len = 0;
	const char *tx = NULL;
	if (!lua_isnoneornil(L, 2)) {
		tx = luaL_checklstring(L, 2, &tx_len);
	}

	lua_Integer rx_len = luaL_optinteger(L, 3, 0);
	if (rx_len < 0 || rx_len > 256) {
		return luaL_error(L, "i2c_tx_rx: read length %d is outside 0..256",
				(int)rx_len);
	}

	if (tx_len == 0 && rx_len == 0) {
		return luaL_error(L, "i2c_tx_rx: nothing to send and nothing to read");
	}

	uint8_t rx[256];
	esp_err_t res;

	if (tx_len > 0 && rx_len > 0) {
		res = i2c_master_write_read_device(LUA_I2C_PORT, (uint8_t)addr,
				(const uint8_t *)tx, tx_len, rx, (size_t)rx_len,
				pdMS_TO_TICKS(I2C_TIMEOUT_MS));
	} else if (tx_len > 0) {
		res = i2c_master_write_to_device(LUA_I2C_PORT, (uint8_t)addr,
				(const uint8_t *)tx, tx_len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
	} else {
		res = i2c_master_read_from_device(LUA_I2C_PORT, (uint8_t)addr,
				rx, (size_t)rx_len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
	}

	if (res != ESP_OK) {
		lua_pushnil(L);
		lua_pushstring(L, esp_err_to_name(res));
		return 2;
	}

	if (rx_len > 0) {
		lua_pushlstring(L, (const char *)rx, (size_t)rx_len);
	} else {
		lua_pushboolean(L, 1);
	}
	return 1;
}

// Probe one address, for scanning a bus.
static int l_i2c_detect_addr(lua_State *L) {
	if (!m_i2c_started) {
		return luaL_error(L, "i2c_detect_addr: call i2c_start first");
	}

	lua_Integer addr = luaL_checkinteger(L, 1);
	if (addr < 0 || addr > 127) {
		return luaL_error(L, "i2c_detect_addr: address %d is outside 0..127",
				(int)addr);
	}

	// A zero-length write is the conventional probe: the address phase alone
	// tells us whether anything acknowledges.
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
	i2c_master_stop(cmd);
	esp_err_t res = i2c_master_cmd_begin(LUA_I2C_PORT, cmd,
			pdMS_TO_TICKS(I2C_TIMEOUT_MS));
	i2c_cmd_link_delete(cmd);

	lua_pushboolean(L, res == ESP_OK);
	return 1;
}

// ------------------------------------------------------------ registration --

/*
 * vesc.pwm_start(freq_hz, duty, channel, pin [, bits]) -> the frequency set
 *
 * LEDC, low speed mode, one timer per channel. duty is 0..1 and is clamped
 * rather than refused, as the lisp binding does.
 *
 * This is how a board with a real backlight drives it, which is why it is the
 * one binding a dash cannot do without: the panel comes up dark until
 * something sets a duty here.
 */
static int l_pwm_start(lua_State *L) {
	uint32_t freq = (uint32_t)luaL_checkinteger(L, 1);
	float duty = (float)luaL_checknumber(L, 2);
	int chan = (int)luaL_checkinteger(L, 3);
	int pin = (int)luaL_checkinteger(L, 4);
	int bits = (int)luaL_optinteger(L, 5, 10);

	utils_truncate_number(&duty, 0.0, 1.0);

	if (chan < 0 || chan >= LEDC_TIMER_MAX) {
		return luaL_error(L, "pwm_start: channel %d is outside 0..%d",
				chan, LEDC_TIMER_MAX - 1);
	}
	if (!utils_gpio_is_valid(pin)) {
		return luaL_error(L, "pwm_start: %d is not a usable pin", pin);
	}
	if (bits < 2 || bits > 14) {
		return luaL_error(L, "pwm_start: %d bits is outside 2..14", bits);
	}

	ledc_timer_config_t timer = {
			.speed_mode = LEDC_LOW_SPEED_MODE,
			.timer_num = chan,
			.duty_resolution = bits,
			.freq_hz = freq,
			.clk_cfg = LEDC_AUTO_CLK,
	};

	if (ledc_timer_config(&timer) != ESP_OK) {
		return luaL_error(L, "pwm_start: %u Hz at %d bits is not achievable",
				(unsigned)freq, bits);
	}

	ledc_channel_config_t ch = {
			.speed_mode = LEDC_LOW_SPEED_MODE,
			.channel = chan,
			.timer_sel = chan,
			.intr_type = LEDC_INTR_DISABLE,
			.gpio_num = pin,
			.duty = (int)(duty * (float)(1 << bits)),
			.hpoint = 0,
	};

	if (ledc_channel_config(&ch) != ESP_OK) {
		return luaL_error(L, "pwm_start: could not configure channel %d", chan);
	}

	lua_pushinteger(L, ledc_get_freq(LEDC_LOW_SPEED_MODE, chan));
	return 1;
}

// vesc.pwm_stop(channel) -- releases the pin, leaving it low.
static int l_pwm_stop(lua_State *L) {
	int chan = (int)luaL_checkinteger(L, 1);

	if (chan < 0 || chan >= LEDC_TIMER_MAX) {
		return luaL_error(L, "pwm_stop: channel %d is outside 0..%d",
				chan, LEDC_TIMER_MAX - 1);
	}

	ledc_stop(LEDC_LOW_SPEED_MODE, chan, 0);
	return 0;
}

static const luaL_Reg io_fns[] = {
	{"pwm_start", l_pwm_start},
	{"pwm_stop", l_pwm_stop},

	{"uart_start", l_uart_start},
	{"uart_stop", l_uart_stop},
	{"uart_write", l_uart_write},
	{"uart_read", l_uart_read},
	{"uart_available", l_uart_available},

	{"i2c_start", l_i2c_start},
	{"i2c_stop", l_i2c_stop},
	{"i2c_tx_rx", l_i2c_tx_rx},
	{"i2c_detect_addr", l_i2c_detect_addr},

	{NULL, NULL},
};

void lua_vesc_io_register(script_lua_t *s) {
	script_lua_register(s, io_fns);
}
