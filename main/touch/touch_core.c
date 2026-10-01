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

#include "touch_core.h"

#include "touch_cst836u.h"
#include "utils.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lcd_axs15231b.h"

#define TOUCH_I2C_PORT I2C_NUM_0
#define TOUCH_EVENT_TASK_STACK 2048
#define TOUCH_EVENT_POLL_MS 50
#define TOUCH_EVENT_MIN_INTERVAL_MS 20
#define TOUCH_EVENT_MOVE_DELTA 2

static SemaphoreHandle_t touch_mutex = 0;
static TaskHandle_t touch_event_task_handle = 0;
static touch_driver_t touch_driver = {0};
static touch_part_t touch_part = TOUCH_PART_NONE;
static esp_lcd_touch_handle_t touch_handle = NULL;
static esp_lcd_panel_io_handle_t touch_io_handle = NULL;
static i2c_port_t touch_i2c_port = TOUCH_I2C_PORT;
static bool touch_owns_i2c_driver = false;
static spi_host_device_t touch_spi_host = SPI2_HOST;
static bool touch_owns_spi_bus = false;
static bool touch_has_int = false;

static touch_core_event_cb_t touch_event_cb = NULL;
static bool (*touch_event_enabled)(void) = NULL;

typedef esp_err_t (*touch_i2c_create_fn_t)(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *cfg, esp_lcd_touch_handle_t *tp);

static void touch_event_task(void *arg);
static bool touch_i2c_addr_present(uint8_t address);
static esp_err_t touch_esp_lcd_deinit(void);

static bool touch_driver_loaded(void) {
	return touch_driver.read_data != 0 && touch_driver.get_data != 0;
}

bool touch_core_gpio_valid_or_nc(int pin) {
	return pin == -1 || utils_gpio_is_valid(pin);
}

bool touch_core_init(void) {
	if (!touch_mutex) {
		touch_mutex = xSemaphoreCreateMutex();
		if (!touch_mutex) {
			return false;
		}
	}

	if (!touch_event_task_handle) {
		if (xTaskCreatePinnedToCore(
				touch_event_task,
				"touch_evt",
				TOUCH_EVENT_TASK_STACK,
				NULL,
				6,
				&touch_event_task_handle,
				tskNO_AFFINITY) != pdPASS) {
			return false;
		}
	}

	return true;
}

static void IRAM_ATTR touch_esp_lcd_interrupt_cb(esp_lcd_touch_handle_t tp) {
	(void)tp;
	touch_core_irq_from_isr();
}

static esp_err_t touch_esp_lcd_deinit(void) {
	esp_err_t first_err = ESP_OK;

	if (touch_handle) {
		esp_err_t res = touch_handle->del(touch_handle);
		if (res != ESP_OK && first_err == ESP_OK) {
			first_err = res;
		}
		touch_handle = NULL;
	}

	if (touch_io_handle) {
		esp_err_t res = esp_lcd_panel_io_del(touch_io_handle);
		if (res != ESP_OK && first_err == ESP_OK) {
			first_err = res;
		}
		touch_io_handle = NULL;
	}

	if (touch_owns_i2c_driver) {
		esp_err_t res = i2c_driver_delete(touch_i2c_port);
		if (res != ESP_OK && first_err == ESP_OK) {
			first_err = res;
		}
	}

	touch_owns_i2c_driver = false;

	if (touch_owns_spi_bus) {
		esp_err_t res = spi_bus_free(touch_spi_host);
		if (res != ESP_OK && first_err == ESP_OK) {
			first_err = res;
		}
	}

	touch_owns_spi_bus = false;
	return first_err;
}

static esp_err_t touch_esp_lcd_read_data(void) {
	if (!touch_handle) {
		return ESP_ERR_INVALID_STATE;
	}

	return esp_lcd_touch_read_data(touch_handle);
}

static esp_err_t touch_esp_lcd_get_data(touch_point_t *data, uint8_t *point_cnt, uint8_t max_point_cnt) {
	if (!touch_handle) {
		return ESP_ERR_INVALID_STATE;
	}

	if (!data || !point_cnt || max_point_cnt == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	uint8_t req_cnt = max_point_cnt;
	if (req_cnt > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
		req_cnt = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
	}

	esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
	uint8_t cnt = 0;
	esp_err_t res = esp_lcd_touch_get_data(touch_handle, points, &cnt, req_cnt);
	if (res != ESP_OK) {
		return res;
	}

	for (uint8_t i = 0;i < cnt;i++) {
		data[i].track_id = points[i].track_id;
		data[i].x = points[i].x;
		data[i].y = points[i].y;
		data[i].strength = points[i].strength;
	}

	*point_cnt = cnt;
	return ESP_OK;
}

static esp_err_t touch_init_i2c_bus(int sda, int scl, uint32_t freq) {
	i2c_config_t i2c_conf = {
			.mode = I2C_MODE_MASTER,
			.sda_io_num = sda,
			.scl_io_num = scl,
			.sda_pullup_en = GPIO_PULLUP_ENABLE,
			.scl_pullup_en = GPIO_PULLUP_ENABLE,
			.master.clk_speed = freq,
	};

	bool reuse_existing_i2c = false;
	esp_err_t cfg_res = i2c_param_config(TOUCH_I2C_PORT, &i2c_conf);
	if (cfg_res == ESP_ERR_INVALID_ARG) {
		i2c_conf.sda_pullup_en = GPIO_PULLUP_DISABLE;
		i2c_conf.scl_pullup_en = GPIO_PULLUP_DISABLE;
		cfg_res = i2c_param_config(TOUCH_I2C_PORT, &i2c_conf);
	}
	if (cfg_res != ESP_OK) {
		if (cfg_res != ESP_FAIL && cfg_res != ESP_ERR_INVALID_STATE) {
			return cfg_res;
		}
		reuse_existing_i2c = true;
	}

	esp_err_t res = i2c_driver_install(TOUCH_I2C_PORT, i2c_conf.mode, 0, 0, 0);
	if (res == ESP_ERR_INVALID_STATE) {
		reuse_existing_i2c = true;
	} else if (res != ESP_OK) {
		return res;
	}

	touch_owns_i2c_driver = !reuse_existing_i2c;
	touch_i2c_port = TOUCH_I2C_PORT;
	return ESP_OK;
}

// Does anything answer at this address? A zero length write is the cheapest
// probe there is: the address byte either gets an ACK or it does not.
static bool touch_i2c_addr_present(uint8_t address) {
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	if (!cmd) {
		return false;
	}

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
	i2c_master_stop(cmd);
	esp_err_t res = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(50));
	i2c_cmd_link_delete(cmd);

	return res == ESP_OK;
}

static esp_err_t touch_init_i2c_esp_lcd(int sda, int scl, int rst, int int_pin,
		uint16_t width, uint16_t height, uint32_t freq,
		esp_lcd_panel_io_i2c_config_t io_conf, void *driver_data,
		touch_i2c_create_fn_t create_fn, touch_driver_t *driver, uint8_t alt_addr) {
	if (!driver || !create_fn) {
		return ESP_ERR_INVALID_ARG;
	}

	if (touch_handle || touch_io_handle) {
		esp_err_t deinit_res = touch_esp_lcd_deinit();
		if (deinit_res != ESP_OK) {
			return deinit_res;
		}
	}

	esp_err_t res = touch_init_i2c_bus(sda, scl, freq);
	if (res != ESP_OK) {
		return res;
	}

	// A part with two possible addresses gets probed rather than assumed. The
	// GT911 picks between 0x5D and 0x14 from its INT level as reset is
	// released, and which one a board lands on is a property of its wiring.
	if (alt_addr && !touch_i2c_addr_present(io_conf.dev_addr) &&
			touch_i2c_addr_present(alt_addr)) {
		io_conf.dev_addr = alt_addr;
		if (driver_data) {
			((esp_lcd_touch_io_gt911_config_t *)driver_data)->dev_addr = alt_addr;
		}
	}

	io_conf.scl_speed_hz = 0;
	res = esp_lcd_new_panel_io_i2c(TOUCH_I2C_PORT, &io_conf, &touch_io_handle);
	if (res != ESP_OK) {
		touch_esp_lcd_deinit();
		return res;
	}

	esp_lcd_touch_config_t tp_cfg = {
			.x_max = width,
			.y_max = height,
			.rst_gpio_num = rst >= 0 ? (gpio_num_t)rst : GPIO_NUM_NC,
			.int_gpio_num = int_pin >= 0 ? (gpio_num_t)int_pin : GPIO_NUM_NC,
			.levels = {
					.reset = 0,
					.interrupt = 0,
			},
			.flags = {
					.swap_xy = 0,
					.mirror_x = 0,
					.mirror_y = 0,
			},
			.interrupt_callback = touch_esp_lcd_interrupt_cb,
			.driver_data = driver_data,
	};

	res = create_fn(touch_io_handle, &tp_cfg, &touch_handle);
	if (res != ESP_OK) {
		touch_esp_lcd_deinit();
		return res;
	}

	touch_has_int = (int_pin >= 0);

	driver->deinit = touch_esp_lcd_deinit;
	driver->read_data = touch_esp_lcd_read_data;
	driver->get_data = touch_esp_lcd_get_data;

	return ESP_OK;
}

/*
 * The GT911 has two possible I2C addresses and picks between them as its reset
 * is released, from the level on its INT pin: 0x5D or 0x14. Which one a board
 * ends up on is a property of its wiring, not of the driver -- the Waveshare
 * ESP32-S3-Touch-LCD-4 answers at 0x14, and driving INT low across the reset
 * does not move it, which was tried.
 *
 * So probe rather than assume. Both addresses are valid for this part, the
 * probe is one byte, and it runs once at load. Without this a board on the
 * second address reports nothing more useful than "Touch not loaded", which is
 * indistinguishable from a broken connector.
 */
#define GT911_ADDR_ALT	0x14

// The CST836U is not an esp_lcd_touch part: it has its own register-level
// driver, and its reset is released here rather than inside a component.
static esp_err_t touch_init_cst836u(int sda, int scl, int rst, uint16_t width,
		uint16_t height, uint32_t freq, touch_driver_t *driver) {
	esp_err_t res = touch_init_i2c_bus(sda, scl, freq);
	if (res != ESP_OK) {
		return res;
	}

	if (rst >= 0) {
		gpio_reset_pin((gpio_num_t)rst);
		gpio_set_direction((gpio_num_t)rst, GPIO_MODE_OUTPUT);
		gpio_set_level((gpio_num_t)rst, 0);
		vTaskDelay(pdMS_TO_TICKS(10));
		gpio_set_level((gpio_num_t)rst, 1);
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	res = touch_cst836u_init(touch_i2c_port, width, height, driver);
	if (res != ESP_OK) {
		return res;
	}

	driver->deinit = touch_esp_lcd_deinit;

	// Interrupt-driven events are not wired up for this part, so the event
	// task polls it. The pin is accepted and ignored rather than refused.
	touch_has_int = false;
	return ESP_OK;
}

static void touch_delete_locked(void) {
	if (touch_driver.deinit) {
		touch_driver.deinit();
	}

	memset(&touch_driver, 0, sizeof(touch_driver));
	touch_part = TOUCH_PART_NONE;
	touch_cst836u_reset();
}

esp_err_t touch_core_load_i2c(touch_part_t part, int sda, int scl, int rst,
		int int_pin, uint16_t width, uint16_t height, uint32_t freq) {
	if (!touch_mutex) {
		return ESP_ERR_INVALID_STATE;
	}

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	touch_delete_locked();
	xSemaphoreGive(touch_mutex);

	touch_driver_t driver = {0};
	esp_err_t res;

	switch (part) {
	case TOUCH_PART_CST816S: {
		esp_lcd_panel_io_i2c_config_t io_conf = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
		res = touch_init_i2c_esp_lcd(sda, scl, rst, int_pin, width, height, freq,
				io_conf, NULL, esp_lcd_touch_new_i2c_cst816s, &driver, 0);
		break;
	}

	case TOUCH_PART_GT911: {
		esp_lcd_panel_io_i2c_config_t io_conf = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
		// The address is probed inside touch_init_i2c_esp_lcd, once its own
		// bus setup has run. Opening the bus here as well installed the
		// driver twice: the second call recovered, but it logged "i2c driver
		// install error" and left touch_owns_i2c_driver false, so the deinit
		// path would not have released what it allocated.
		esp_lcd_touch_io_gt911_config_t gt911_cfg = {
				.dev_addr = io_conf.dev_addr,
		};
		res = touch_init_i2c_esp_lcd(sda, scl, rst, int_pin, width, height, freq,
				io_conf, &gt911_cfg, esp_lcd_touch_new_i2c_gt911, &driver,
				GT911_ADDR_ALT);
		break;
	}

	case TOUCH_PART_CST9217: {
		esp_lcd_panel_io_i2c_config_t io_conf = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
		res = touch_init_i2c_esp_lcd(sda, scl, rst, int_pin, width, height, freq,
				io_conf, NULL, esp_lcd_touch_new_i2c_cst9217, &driver, 0);
		break;
	}

	case TOUCH_PART_AXS15231: {
		esp_lcd_panel_io_i2c_config_t io_conf = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
		res = touch_init_i2c_esp_lcd(sda, scl, rst, int_pin, width, height, freq,
				io_conf, NULL, esp_lcd_touch_new_i2c_axs15231b, &driver, 0);
		break;
	}

	case TOUCH_PART_CST836U:
		res = touch_init_cst836u(sda, scl, rst, width, height, freq, &driver);
		break;

	default:
		return ESP_ERR_INVALID_ARG;
	}

	if (res != ESP_OK) {
		return res;
	}

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	touch_driver = driver;
	touch_part = part;
	xSemaphoreGive(touch_mutex);

	return ESP_OK;
}

esp_err_t touch_core_load_spi(touch_part_t part, int host, int mosi, int miso,
		int sclk, int cs, int int_pin, uint16_t width, uint16_t height,
		uint32_t freq) {
	if (part != TOUCH_PART_XPT2046) {
		return ESP_ERR_INVALID_ARG;
	}

	if (!touch_mutex) {
		return ESP_ERR_INVALID_STATE;
	}

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	touch_delete_locked();
	xSemaphoreGive(touch_mutex);

	if (touch_handle || touch_io_handle) {
		esp_err_t deinit_res = touch_esp_lcd_deinit();
		if (deinit_res != ESP_OK) {
			return deinit_res;
		}
	}

	spi_host_device_t spi_host = (spi_host_device_t)host;
	spi_bus_config_t bus_cfg = {
			.mosi_io_num = mosi,
			.miso_io_num = miso,
			.sclk_io_num = sclk,
			.quadwp_io_num = -1,
			.quadhd_io_num = -1,
			.max_transfer_sz = 0,
	};

	bool reuse_existing_spi = false;
	esp_err_t res = spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
	if (res == ESP_ERR_INVALID_STATE) {
		reuse_existing_spi = true;
	} else if (res != ESP_OK) {
		return res;
	}

	touch_spi_host = spi_host;
	touch_owns_spi_bus = !reuse_existing_spi;

	esp_lcd_panel_io_spi_config_t io_conf = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(cs);
	io_conf.pclk_hz = freq;
	res = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_conf, &touch_io_handle);
	if (res != ESP_OK) {
		touch_esp_lcd_deinit();
		return res;
	}

	esp_lcd_touch_config_t tp_cfg = {
			.x_max = width,
			.y_max = height,
			.rst_gpio_num = GPIO_NUM_NC,
			.int_gpio_num = int_pin >= 0 ? (gpio_num_t)int_pin : GPIO_NUM_NC,
			.levels = {
					.reset = 0,
					.interrupt = 0,
			},
			.flags = {
					.swap_xy = 0,
					.mirror_x = 0,
					.mirror_y = 0,
			},
			.interrupt_callback = touch_esp_lcd_interrupt_cb,
	};

	res = esp_lcd_touch_new_spi_xpt2046(touch_io_handle, &tp_cfg, &touch_handle);
	if (res != ESP_OK) {
		touch_esp_lcd_deinit();
		return res;
	}

	touch_has_int = (int_pin >= 0);

	touch_driver_t driver = {
			.deinit = touch_esp_lcd_deinit,
			.read_data = touch_esp_lcd_read_data,
			.get_data = touch_esp_lcd_get_data,
	};

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	touch_driver = driver;
	touch_part = part;
	xSemaphoreGive(touch_mutex);

	return ESP_OK;
}

bool touch_core_loaded(void) {
	if (!touch_mutex) {
		return false;
	}

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	bool loaded = touch_driver_loaded();
	xSemaphoreGive(touch_mutex);
	return loaded;
}

touch_part_t touch_core_part(void) {
	return touch_part;
}

bool touch_core_lock(void) {
	if (!touch_mutex) {
		return false;
	}

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	if (!touch_driver_loaded()) {
		xSemaphoreGive(touch_mutex);
		return false;
	}

	return true;
}

void touch_core_unlock(void) {
	if (touch_mutex) {
		xSemaphoreGive(touch_mutex);
	}
}

/*
 * Read tallies. Plain uint32 without a lock: they are only ever incremented
 * inside the read lock and read for diagnostics, so a torn read reports a
 * number one off rather than mattering.
 */
static uint32_t m_read_ok;
static uint32_t m_read_err;
static esp_err_t m_read_last_err;

esp_err_t touch_core_read(touch_point_t *points, uint8_t *point_cnt, uint8_t max_point_cnt) {
	if (!points || !point_cnt || max_point_cnt == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	if (!touch_core_lock()) {
		return ESP_ERR_INVALID_STATE;
	}

	esp_err_t res = touch_driver.read_data();
	if (res == ESP_OK) {
		res = touch_driver.get_data(points, point_cnt, max_point_cnt);
	}

	// Counted, because the binding above this reports a failed read as "not
	// touched" so a script can poll unconditionally on a board whose panel
	// did not come up. That is the right default and it makes a controller
	// that has stopped answering indistinguishable from a finger that is not
	// there -- which is the one thing worth knowing when touch stops working
	// on a board whose only input is touch.
	if (res == ESP_OK) {
		m_read_ok++;
	} else {
		m_read_err++;
		m_read_last_err = res;
	}

	touch_core_unlock();
	return res;
}

void touch_core_read_stats(uint32_t *ok, uint32_t *err, int *last_err) {
	if (ok) {
		*ok = m_read_ok;
	}
	if (err) {
		*err = m_read_err;
	}
	if (last_err) {
		*last_err = (int)m_read_last_err;
	}
}

esp_err_t touch_core_set_transforms(bool swap_xy, bool mirror_x, bool mirror_y) {
	if (!touch_core_lock()) {
		return ESP_ERR_INVALID_STATE;
	}

	touch_cst836u_set_transforms(swap_xy, mirror_x, mirror_y);

	if (!touch_handle) {
		// The CST836U applies them itself, in its own read path.
		touch_core_unlock();
		return ESP_OK;
	}

	// esp_lcd_touch computes the mirror in the controller's native frame,
	// before the swap, so a part whose driver has no set_swap_xy needs the
	// two mirrors exchanged to end up where the caller asked for.
	bool apply_mirror_x = mirror_x;
	bool apply_mirror_y = mirror_y;

	if (swap_xy && touch_handle->set_swap_xy == NULL) {
		apply_mirror_x = mirror_y;
		apply_mirror_y = mirror_x;
	}

	esp_err_t res = esp_lcd_touch_set_swap_xy(touch_handle, swap_xy);
	if (res == ESP_OK) {
		res = esp_lcd_touch_set_mirror_x(touch_handle, apply_mirror_x);
	}
	if (res == ESP_OK) {
		res = esp_lcd_touch_set_mirror_y(touch_handle, apply_mirror_y);
	}

	touch_core_unlock();
	return res;
}

void touch_core_delete(void) {
	if (!touch_mutex) {
		return;
	}

	xSemaphoreTake(touch_mutex, portMAX_DELAY);
	touch_delete_locked();
	xSemaphoreGive(touch_mutex);
}

void touch_core_set_event_cb(touch_core_event_cb_t cb, bool (*enabled)(void)) {
	touch_event_cb = cb;
	touch_event_enabled = enabled;
}

void touch_core_irq_from_isr(void) {
	if (!touch_event_task_handle) {
		return;
	}

	BaseType_t wake = pdFALSE;
	vTaskNotifyGiveFromISR(touch_event_task_handle, &wake);
	portYIELD_FROM_ISR(wake);
}

// Reads the callback once and hands it back, so the task cannot test one
// pointer and then call another if an engine swaps its sink mid-poll.
static touch_core_event_cb_t touch_event_sink(void) {
	touch_core_event_cb_t cb = touch_event_cb;
	if (!cb) {
		return NULL;
	}

	bool (*enabled)(void) = touch_event_enabled;
	if (enabled && !enabled()) {
		return NULL;
	}

	return cb;
}

static bool touch_point_changed_enough(const touch_point_t *prev, const touch_point_t *curr) {
	if (!prev || !curr) {
		return false;
	}

	int dx = (int)curr->x - (int)prev->x;
	if (dx < 0) {
		dx = -dx;
	}

	int dy = (int)curr->y - (int)prev->y;
	if (dy < 0) {
		dy = -dy;
	}

	return dx >= TOUCH_EVENT_MOVE_DELTA ||
			dy >= TOUCH_EVENT_MOVE_DELTA ||
			curr->track_id != prev->track_id ||
			curr->strength != prev->strength;
}

static void touch_event_task(void *arg) {
	(void)arg;
	const TickType_t min_emit_ticks = pdMS_TO_TICKS(TOUCH_EVENT_MIN_INTERVAL_MS);
	const TickType_t poll_delay_ticks = pdMS_TO_TICKS(TOUCH_EVENT_POLL_MS);
	bool last_pressed = false;
	bool has_last_point = false;
	touch_point_t last_point = {0};
	TickType_t last_emit_tick = 0;

	for (;;) {
		if (touch_has_int) {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
				// Drain pending notifications to coalesce interrupt bursts.
			}
		} else {
			vTaskDelay(poll_delay_ticks);
		}

		if (!touch_event_sink() || !touch_mutex) {
			continue;
		}

		touch_point_t point = {0};
		bool pressed = false;

		xSemaphoreTake(touch_mutex, portMAX_DELAY);
		if (touch_driver_loaded()) {
			esp_err_t res = touch_driver.read_data();
			if (res == ESP_OK) {
				uint8_t point_cnt = 0;
				if (touch_driver.get_data(&point, &point_cnt, 1) == ESP_OK) {
					pressed = point_cnt > 0;
				}
			}
		}
		touch_part_t part = touch_part;
		xSemaphoreGive(touch_mutex);

		touch_core_event_cb_t cb = touch_event_sink();
		if (cb) {
			TickType_t now = xTaskGetTickCount();
			bool state_changed = pressed != last_pressed;
			bool moved = pressed && has_last_point && touch_point_changed_enough(&last_point, &point);
			bool have_interval = (now - last_emit_tick) >= min_emit_ticks;
			bool should_emit = state_changed || (moved && have_interval);

			if (should_emit) {
				cb(pressed, &point, part);
				last_emit_tick = now;
				last_pressed = pressed;
				if (pressed) {
					last_point = point;
					has_last_point = true;
				} else {
					has_last_point = false;
				}
			}
		}
	}
}
