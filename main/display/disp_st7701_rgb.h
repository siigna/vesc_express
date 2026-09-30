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

#ifndef MAIN_DISPLAY_DISP_ST7701_RGB_H_
#define MAIN_DISPLAY_DISP_ST7701_RGB_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "soc/soc_caps.h"
#include "tinygfx.h"
#include "esp_lcd_st7701.h"

#if SOC_LCD_RGB_SUPPORTED

typedef struct {
	int width;
	int height;

	// Register configuration link (3-wire SPI, software driven)
	int pin_cs;
	int pin_sclk;
	int pin_sda;
	int pin_rst;		// -1 if the panel reset is not wired

	// RGB bus
	int pin_de;
	int pin_vsync;
	int pin_hsync;
	int pin_pclk;
	int pin_data[16];	// B0..B4, G0..G5, R0..R4

	int pclk_hz;		// 0 for the panel default (16 MHz)

	/*
	 * Blanking intervals. Leave hsync_pulse_width at 0 to take the ST7701
	 * component's generic 480x480 defaults.
	 *
	 * Worth setting from the board: the defaults are generic for the
	 * controller, not for a particular panel, and a panel given blanking it
	 * does not expect simply does not sync. It stays dark with no error
	 * anywhere, because on an RGB panel a draw only writes a framebuffer and
	 * nothing in the stack can see whether the glass is scanning it out.
	 */
	int hsync_pulse_width;
	int hsync_back_porch;
	int hsync_front_porch;
	int vsync_pulse_width;
	int vsync_back_porch;
	int vsync_front_porch;

	/*
	 * Panel register initialisation, NULL to take the ST7701 component's
	 * built-in sequence.
	 *
	 * Set this from the board too, and for the same reason as the blanking
	 * above: the component's default table is one manufacturer's panel, not
	 * every panel with an ST7701 on it. Gamma and power differ, and so do
	 * two registers that decide what the bus even means -- 0xCD, which maps
	 * the parallel data lines onto colour bits, and 0x3A, the pixel format.
	 * Get those wrong and the panel scans out perfectly while showing the
	 * wrong colours, with nothing to report.
	 */
	const st7701_lcd_init_cmd_t *init_cmds;
	uint16_t init_cmds_size;

	/*
	 * Lines of bounce buffer in internal RAM, 0 for none.
	 *
	 * A bounce buffer lets the scanout run ahead of PSRAM latency and so
	 * allows a higher pixel clock, but it is refilled from an interrupt and
	 * the IDF documents that DMA and the LCD can fall out of step in that
	 * mode, leaving the image *permanently* shifted. With a single PSRAM
	 * framebuffer there is no refill and nothing to desynchronise. 480x480
	 * at 16 MHz is 32 MB/s, which octal PSRAM serves without help, so prefer
	 * 0 unless a panel actually needs the headroom.
	 */
	int bounce_lines;
} disp_st7701_rgb_cfg_t;

bool disp_st7701_rgb_init(const disp_st7701_rgb_cfg_t *cfg);
void disp_st7701_rgb_deinit(void);
void disp_st7701_rgb_set_rotation(int rotation);
/*
 * Send a raw command to the panel over the 3-wire SPI register link.
 *
 * This exists for bring-up. A parallel RGB panel reports nothing about
 * itself, so when the picture is geometrically right but tonally wrong the
 * only way to tell inversion from gamma from VCOM is to change one register
 * at a time and look. Reflashing to try each value is minutes per attempt;
 * this makes it seconds.
 */
bool disp_st7701_rgb_cmd(uint8_t cmd, const uint8_t *data, size_t len);

bool disp_st7701_rgb_render_image(image_buffer_t *img, uint16_t x, uint16_t y, color_t *colors);
void disp_st7701_rgb_clear(uint32_t color);
void disp_st7701_rgb_reset(void);

#endif

#endif /* MAIN_DISPLAY_DISP_ST7701_RGB_H_ */
