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

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CH32_I2C_PORT		I2C_NUM_0
#define CH32_I2C_HZ			400000

static const char *TAG = "ws_s3_lcd4";

/*
 * Free a shared I2C bus that was left mid-transaction.
 *
 * A soft reset can land while a slave is driving SDA low, and a slave holding
 * the line means the ESP32's controller never gets a start condition -- so the
 * CH32 is unreachable and the whole display stays dark, with nothing to
 * indicate why. Nine clocks let any slave finish the byte it thought it was
 * sending, then a manual stop puts the bus back in idle. From Waveshare's own
 * recovery routine, which exists for the same reason.
 */
static void ch32_recover_bus(void) {
	gpio_config_t in_cfg = {
		.pin_bit_mask = (1ULL << TOUCH_SDA),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};
	gpio_config(&in_cfg);

	gpio_set_direction(TOUCH_SCL, GPIO_MODE_OUTPUT_OD);
	gpio_set_level(TOUCH_SCL, 1);
	esp_rom_delay_us(10);

	for (int i = 0; i < 9 && gpio_get_level(TOUCH_SDA) == 0; i++) {
		gpio_set_level(TOUCH_SCL, 0);
		esp_rom_delay_us(10);
		gpio_set_level(TOUCH_SCL, 1);
		esp_rom_delay_us(10);
	}

	// A stop condition: SDA released while SCL is high.
	gpio_set_direction(TOUCH_SDA, GPIO_MODE_OUTPUT_OD);
	gpio_set_level(TOUCH_SDA, 0);
	esp_rom_delay_us(10);
	gpio_set_level(TOUCH_SCL, 1);
	esp_rom_delay_us(10);
	gpio_set_level(TOUCH_SDA, 1);
	esp_rom_delay_us(10);

	gpio_set_direction(TOUCH_SDA, GPIO_MODE_INPUT);
	gpio_set_direction(TOUCH_SCL, GPIO_MODE_INPUT);
}

static esp_err_t ch32_write(uint8_t reg, uint8_t val) {
	const uint8_t buf[2] = {reg, val};
	return i2c_master_write_to_device(CH32_I2C_PORT, CH32_ADDR, buf,
			sizeof(buf), pdMS_TO_TICKS(50));
}

/*
 * Bring up the panel's power and release the resets.
 *
 * Runs from hw_init, so it is done before anything can call disp-init: the
 * ST7701 latches its configuration out of the SPI sequence that
 * disp_st7701_rgb_init sends, and a panel that has not been reset first does
 * not take it. That is what a screen showing the right shapes in the wrong
 * colours looks like.
 *
 * The bus is released afterwards, because LispBM's i2c-start claims the same
 * port and would fail if this held it.
 *
 * Ordering and the 200 ms waits are the vendor's. The buzzer bit is written
 * zero in both steps and never one -- writing this chip with another part's
 * register map is how you make it scream and not stop, since the register that
 * silences a TCA9554 does not exist here.
 */
static void ch32_display_power_on(void) {
	ch32_recover_bus();

	const i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = TOUCH_SDA,
		.scl_io_num = TOUCH_SCL,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = CH32_I2C_HZ,
	};

	if (i2c_param_config(CH32_I2C_PORT, &conf) != ESP_OK ||
			i2c_driver_install(CH32_I2C_PORT, conf.mode, 0, 0, 0) != ESP_OK) {
		ESP_LOGE(TAG, "could not open the CH32 bus; display will stay dark");
		return;
	}

	bool ok = false;

	for (int attempt = 0; attempt < 3 && !ok; attempt++) {
		ok = ch32_write(CH32_REG_DIRECTION, CH32_DIR_DEFAULT) == ESP_OK &&
			 ch32_write(CH32_REG_OUTPUT, CH32_OUT_RESET) == ESP_OK;

		if (!ok) {
			vTaskDelay(pdMS_TO_TICKS(20));
			continue;
		}

		vTaskDelay(pdMS_TO_TICKS(200));

		/*
		 * The GT911 reads its own INT pin as the reset comes up and takes its
		 * I2C address from it: low selects 0x5D, high selects 0x14. INT has a
		 * pull-up here, so a board left to itself answers at 0x14 -- and the
		 * esp_lcd_touch driver is configured for 0x5D, which is why touch was
		 * never found even once the reset was released.
		 *
		 * Driving INT low across the release picks 0x5D, so the address the
		 * driver already expects is the one the chip uses and the shared touch
		 * code needs no board-specific case.
		 */
		gpio_set_direction(TOUCH_INT, GPIO_MODE_OUTPUT);
		gpio_set_level(TOUCH_INT, 0);
		esp_rom_delay_us(100);

		ok = ch32_write(CH32_REG_DIRECTION, CH32_DIR_DEFAULT) == ESP_OK &&
			 ch32_write(CH32_REG_OUTPUT, CH32_OUT_RUN) == ESP_OK;

		// Held past the release, then handed back as the input the driver
		// attaches its interrupt to.
		vTaskDelay(pdMS_TO_TICKS(10));
		gpio_set_direction(TOUCH_INT, GPIO_MODE_INPUT);

		if (ok) {
			vTaskDelay(pdMS_TO_TICKS(200));
			// Backlight: 0 is brightest. See CH32_PWM_BRIGHTEST.
			ch32_write(CH32_REG_PWM, CH32_PWM_BRIGHTEST);
		} else {
			vTaskDelay(pdMS_TO_TICKS(20));
		}
	}

	if (ok) {
		ESP_LOGI(TAG, "CH32 up: panel powered, resets released");
	} else {
		ESP_LOGE(TAG, "CH32 did not answer; display and touch will not work");
	}

	i2c_driver_delete(CH32_I2C_PORT);
}

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
		.hsync_pulse_width  = DISP_HSYNC_PULSE,
		.hsync_back_porch   = DISP_HSYNC_BACK_PORCH,
		.hsync_front_porch  = DISP_HSYNC_FRONT_PORCH,
		.vsync_pulse_width  = DISP_VSYNC_PULSE,
		.vsync_back_porch   = DISP_VSYNC_BACK_PORCH,
		.vsync_front_porch  = DISP_VSYNC_FRONT_PORCH,
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

/*
 * Set to 0 to leave the CH32 entirely alone at boot.
 *
 * Useful for two things. It bisects a fault against a firmware that does not
 * touch the chip, and -- because the CH32 has its own supply and does not reset
 * when the ESP32 does -- it lets the configuration another firmware left behind
 * be read back intact. Restore the factory image, let it light the panel, flash
 * a build with this at 0, and the registers still hold whatever the working
 * firmware set.
 */
#define CH32_INIT_AT_BOOT		1

void hw_init(void) {
	// Before the lisp starts, so a package calling disp-init finds a panel
	// that has been reset and powered.
#if CH32_INIT_AT_BOOT
	ch32_display_power_on();
#endif

	lispif_add_ext_load_callback(load_extensions);
}
