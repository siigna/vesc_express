/*
	Copyright 2025 Benjamin Vedder	benjamin@vedder.se

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
 * ST7701 over a parallel RGB bus.
 *
 * disp_st7701.c drives the same controller over MIPI-DSI, which only exists on
 * the ESP32-P4. Cheaper 480x480 boards (Waveshare ESP32-S3-Touch-LCD-4 and
 * friends) wire the panel to the S3's LCD_CAM RGB peripheral instead, and
 * configure its registers over a 3-wire SPI link beforehand. Espressif's
 * esp_lcd_st7701 component handles both; this file wires up the RGB half.
 *
 * The framebuffer is 480 * 480 * 2 = 460800 bytes, so PSRAM is required.
 */

#include "disp_st7701_rgb.h"

#if SOC_LCD_RGB_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(SCRIPT_ENGINE_LISP)
#include "lispif.h"
#endif /* SCRIPT_ENGINE_LISP */
#include "lispbm.h"
#include <string.h>

#define TAG "ST7701RGB"

#define CHUNK_LINES	40

static int m_width  = 0;
static int m_height = 0;
static int m_rotation = 0;

static esp_lcd_panel_io_handle_t m_io    = NULL;
static esp_lcd_panel_handle_t    m_panel = NULL;
static uint8_t *m_pix_buf = NULL;
static size_t m_pix_buf_bytes = 0;

static inline uint16_t rgb888_to_rgb565(uint32_t rgb) {
    uint16_t r = (uint16_t)((rgb >> 19) & 0x1F);
    uint16_t g = (uint16_t)((rgb >> 10) & 0x3F);
    uint16_t b = (uint16_t)((rgb >> 3) & 0x1F);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void rotate_rgb565(const uint16_t *src, uint16_t *dst, int w, int h, int rotation) {
    if (rotation == 1) { // 90° CW: dst is h×w
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                dst[(uint32_t)x * h + (h - 1 - y)] = src[(uint32_t)y * w + x];
            }
        }
    } else if (rotation == 2) { // 180°
        int total = w * h;
        for (int i = 0; i < total; i++) {
            dst[total - 1 - i] = src[i];
        }
    } else if (rotation == 3) { // 270° CW: dst is h×w
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                dst[(uint32_t)(w - 1 - x) * h + y] = src[(uint32_t)y * w + x];
            }
        }
    }
}

/*
 * Convert a horizontal band of an image to rgb565.
 *
 * start and count are in pixels from the top left of the image, so a caller
 * can work through a tall image in pieces that fit the scratch buffer. The
 * source index stays absolute -- the packed formats address their bits by it,
 * and COLOR_TO_RGB888 needs the real x and y for a gradient -- while the
 * destination is written from zero.
 */
static bool convert_to_rgb565(const image_buffer_t *img, color_t *colors, uint16_t *dst,
        uint32_t start, uint32_t count) {
    const uint32_t end = start + count;

    switch (img->fmt) {
    case indexed2:
        if (!colors) {
            return false;
        }
        for (uint32_t i = start; i < end; i++) {
            uint32_t byte = i >> 3;
            uint32_t bit = 7U - (i & 0x7U);
            uint32_t color_ind = (img->data[byte] >> bit) & 0x1U;
            uint32_t rgb = COLOR_TO_RGB888(colors[color_ind], i % img->width, i / img->width);
            dst[i - start] = rgb888_to_rgb565(rgb);
        }
        return true;

    case indexed4:
        if (!colors) {
            return false;
        }
        for (uint32_t i = start; i < end; i++) {
            uint32_t byte = i >> 2;
            uint32_t bit = (3U - (i & 0x3U)) * 2U;
            uint32_t color_ind = (img->data[byte] >> bit) & 0x3U;
            uint32_t rgb = COLOR_TO_RGB888(colors[color_ind], i % img->width, i / img->width);
            dst[i - start] = rgb888_to_rgb565(rgb);
        }
        return true;

    case indexed16:
        if (!colors) {
            return false;
        }
        for (uint32_t i = start; i < end; i++) {
            uint32_t byte = i >> 1;
            uint32_t bit = (1U - (i & 0x1U)) * 4U;
            uint32_t color_ind = (img->data[byte] >> bit) & 0xFU;
            uint32_t rgb = COLOR_TO_RGB888(colors[color_ind], i % img->width, i / img->width);
            dst[i - start] = rgb888_to_rgb565(rgb);
        }
        return true;

    case rgb332:
        for (uint32_t i = start; i < end; i++) {
            uint8_t pix = img->data[i];
            uint32_t r = (uint32_t)((pix >> 5) & 0x7U);
            uint32_t g = (uint32_t)((pix >> 2) & 0x7U);
            uint32_t b = (uint32_t)(pix & 0x3U);
            uint32_t rgb = (r << 21) | (g << 13) | (b << 6);
            dst[i - start] = rgb888_to_rgb565(rgb);
        }
        return true;

    case rgb565:
        for (uint32_t i = start; i < end; i++) {
            dst[i - start] = (uint16_t)(((uint16_t)img->data[2 * i] << 8) | (uint16_t)img->data[2 * i + 1]);
        }
        return true;

    case rgb888:
        for (uint32_t i = start; i < end; i++) {
            uint32_t rgb = ((uint32_t)img->data[3 * i] << 16) |
                           ((uint32_t)img->data[3 * i + 1] << 8) |
                           (uint32_t)img->data[3 * i + 2];
            dst[i - start] = rgb888_to_rgb565(rgb);
        }
        return true;

    default:
        return false;
    }
}

// An RGB panel owns its framebuffer, so draw_bitmap is a copy into it and
// returns once done. There is no transfer-complete callback to wait on, unlike
// the SPI/QSPI drivers.
static bool draw_rgb565(int x, int y, int w, int h, const void *pixels) {
	if (!m_panel || !pixels) {
		return false;
	}
	return esp_lcd_panel_draw_bitmap(m_panel, x, y, x + w, y + h, pixels) == ESP_OK;
}

bool disp_st7701_rgb_render_image(image_buffer_t *img, uint16_t x, uint16_t y, color_t *colors) {
	if (!m_panel) {
		return false;
	}

	int iw = img->width, ih = img->height;

	// Rotation swaps the on-screen footprint
	int dw = (m_rotation == 1 || m_rotation == 3) ? ih : iw;
	int dh = (m_rotation == 1 || m_rotation == 3) ? iw : ih;
	if (x + dw > m_width || y + dh > m_height) {
		return false;
	}

	if (!m_pix_buf) {
		return false;
	}

	/*
	 * Bring-up logging for the first few renders after a boot. A parallel RGB
	 * panel tells you nothing, so when a picture comes out wrong the only way
	 * to separate "the driver produced the wrong pixels" from "the panel
	 * showed the right pixels wrongly" is to print what was produced. Costs
	 * nothing in a shipping build, where the log level is 0.
	 */
	static int log_renders = 4;
	if (log_renders > 0) {
		log_renders--;
		ESP_LOGI(TAG, "render fmt=%d %dx%d at %d,%d rot=%d img=%p (align %u) buf=%p (align %u)",
				(int)img->fmt, iw, ih, (int)x, (int)y, m_rotation,
				img->data, (unsigned)((uintptr_t)img->data & 63U),
				m_pix_buf, (unsigned)((uintptr_t)m_pix_buf & 63U));
		if (colors) {
			for (int i = 0; i < 4; i++) {
				uint32_t rgb = COLOR_TO_RGB888(colors[i], 0, 0);
				ESP_LOGI(TAG, "  palette[%d] rgb888=%06lx rgb565=%04x",
						i, (unsigned long)rgb, rgb888_to_rgb565(rgb));
			}
		}
	}

	/*
	 * Already in the panel's format and orientation. It still goes through
	 * the scratch buffer rather than straight from the image, because the
	 * image belongs to LispBM: its address has whatever alignment the
	 * allocator happened to give it, and the panel wants a 64-byte aligned
	 * source. Copying a band at a time is cheap next to getting this wrong.
	 */
	if (m_rotation == 0 && img->fmt == rgb565) {
		int band = (int)(m_pix_buf_bytes / (2U * (uint32_t)iw));
		if (band > ih) {
			band = ih;
		}
		if (band < 1) {
			return false;
		}

		for (int row = 0; row < ih; row += band) {
			int rows = (row + band <= ih) ? band : (ih - row);
			memcpy(m_pix_buf, img->data + (size_t)row * (size_t)iw * 2U,
					(size_t)rows * (size_t)iw * 2U);
			if (!draw_rgb565(x, y + row, iw, rows, m_pix_buf)) {
				return false;
			}
		}

		return true;
	}

	uint16_t *buf = (uint16_t *)m_pix_buf;
	uint32_t num_pix = (uint32_t)iw * (uint32_t)ih;

	/*
	 * Unrotated, the image is converted and pushed a band at a time, so only
	 * the band has to fit the scratch buffer rather than the whole image.
	 *
	 * It used to require the lot, and silently refused anything bigger: with a
	 * 480 px wide panel and a 40 line buffer the limit was 38400 pixels, which
	 * a full width strip taller than 80 lines exceeds. That is most of what a
	 * dash draws -- the speed readout and the splash among them -- so the
	 * background cleared and the large elements simply never appeared. A black
	 * screen with a few small pills on it is what that looks like, and nothing
	 * reported an error the display could not already have reported.
	 */
	if (m_rotation == 0) {
		int band = (int)(m_pix_buf_bytes / (2U * (uint32_t)iw));
		if (band > ih) {
			band = ih;
		}
		if (band < 1) {
			return false;
		}

		for (int row = 0; row < ih; row += band) {
			int rows = (row + band <= ih) ? band : (ih - row);
			if (!convert_to_rgb565(img, colors, buf,
					(uint32_t)row * (uint32_t)iw, (uint32_t)rows * (uint32_t)iw)) {
				return false;
			}
			if (row == 0 && log_renders >= 0 && ih > 1) {
				ESP_LOGI(TAG, "  converted row0: [0]=%04x [iw/2]=%04x [iw-1]=%04x",
						buf[0], buf[iw / 2], buf[iw - 1]);
			}
			if (!draw_rgb565(x, y + row, iw, rows, buf)) {
				return false;
			}
		}

		return true;
	}

	// Rotated: the whole image is needed at once to turn it, plus room for the
	// turned copy. Banding that is not worth the complexity, so it still
	// refuses rather than drawing something wrong.
	if ((num_pix * 4U) > m_pix_buf_bytes) {
		return false;
	}

	if (!convert_to_rgb565(img, colors, buf, 0, num_pix)) {
		return false;
	}

	uint16_t *rot = buf + num_pix;
	rotate_rgb565(buf, rot, iw, ih, m_rotation);
	return draw_rgb565(x, y, dw, dh, rot);
}

void disp_st7701_rgb_clear(uint32_t color) {
	if (!m_panel || !m_pix_buf) {
		return;
	}

	uint16_t c = rgb888_to_rgb565(color);
	uint16_t *buf = (uint16_t *)m_pix_buf;
	int lines = (int)(m_pix_buf_bytes / ((size_t)m_width * 2U));
	if (lines > CHUNK_LINES) {
		lines = CHUNK_LINES;
	}
	if (lines < 1) {
		return;
	}

	for (int i = 0; i < m_width * lines; i++) {
		buf[i] = c;
	}

	for (int yy = 0; yy < m_height; yy += lines) {
		int now = m_height - yy;
		if (now > lines) {
			now = lines;
		}
		draw_rgb565(0, yy, m_width, now, buf);
	}
}

void disp_st7701_rgb_reset(void) {
	if (m_panel) {
		esp_lcd_panel_reset(m_panel);
		esp_lcd_panel_init(m_panel);
	}
}

void disp_st7701_rgb_set_rotation(int rotation) {
	m_rotation = rotation & 3;
}

// The panel is square, so a rotation never changes the usable width or
// height. Only the blit path has to care.
/*
 * The LispBM-specific parts of this driver are guarded so the driver itself
 * can be built for either script engine. Only these extension wrappers ever
 * needed the interpreter: the panel code below, and the render/clear/reset
 * callbacks, use image_buffer_t and color_t from tinygfx and know nothing
 * about a script engine.
 */
#if defined(SCRIPT_ENGINE_LISP)
static lbm_value ext_disp_orientation(lbm_value *args, lbm_uint argn) {
	LBM_CHECK_ARGN_NUMBER(1);
	m_rotation = (int)(lbm_dec_as_u32(args[0]) & 3U);
	return ENC_SYM_TRUE;
}
#endif /* SCRIPT_ENGINE_LISP */

bool disp_st7701_rgb_init(const disp_st7701_rgb_cfg_t *cfg) {
	if (!cfg) {
		return false;
	}
	disp_st7701_rgb_deinit();

	m_width  = cfg->width;
	m_height = cfg->height;
	m_rotation = 0;

	// Register configuration goes over a 3-wire SPI link before the RGB bus
	// starts streaming.
	spi_line_config_t line_cfg = {
		.cs_io_type    = IO_TYPE_GPIO,
		.cs_gpio_num   = cfg->pin_cs,
		.scl_io_type   = IO_TYPE_GPIO,
		.scl_gpio_num  = cfg->pin_sclk,
		.sda_io_type   = IO_TYPE_GPIO,
		.sda_gpio_num  = cfg->pin_sda,
		.io_expander   = NULL,
	};
	esp_lcd_panel_io_3wire_spi_config_t io_cfg = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_cfg, 0);
	if (esp_lcd_new_panel_io_3wire_spi(&io_cfg, &m_io) != ESP_OK) {
		ESP_LOGE(TAG, "3-wire SPI io failed");
		return false;
	}

	esp_lcd_rgb_panel_config_t rgb_cfg = {
		.clk_src = LCD_CLK_SRC_DEFAULT,
		.data_width = 16,
		.bits_per_pixel = 16,
		.psram_trans_align = 64,
		.num_fbs = 1,
		.bounce_buffer_size_px = cfg->bounce_lines > 0 ?
				(size_t)cfg->width * (size_t)cfg->bounce_lines : 0,
		.de_gpio_num    = cfg->pin_de,
		.pclk_gpio_num  = cfg->pin_pclk,
		.vsync_gpio_num = cfg->pin_vsync,
		.hsync_gpio_num = cfg->pin_hsync,
		.disp_gpio_num  = -1,
		/*
		 * A framebuffer in PSRAM is refilled into the bounce buffer from an
		 * interrupt, and that interrupt cannot run while the external memory
		 * cache is disabled -- which is exactly what a write to the main flash
		 * does. The IDF states the constraint outright: the LCD "CANNOT
		 * function if the external memory cache is disabled, such as during OTA
		 * or NVS writes to the main flash."
		 *
		 * On the Waveshare S3 Touch LCD 4 that showed up as a repeatable
		 * "Cache error / MMU entry fault" boot loop, because the dash package
		 * writes roughly seventy-five eeprom entries when it restores defaults
		 * and every one of them is a flash operation while this panel refreshes.
		 *
		 * A board that drives its panel without a bounce buffer has no
		 * refill interrupt, but the framebuffer still lives in PSRAM and is
		 * still read continuously, so the option below is what makes flash
		 * writes safe either way.
		 *
		 * The fix is not in this file: the board's sdkconfig enables
		 * CONFIG_SPIRAM_XIP_FROM_PSRAM, which keeps the cache active across a
		 * flash write by running code out of PSRAM. A board that drives an RGB
		 * panel from PSRAM and also writes flash at runtime needs that option,
		 * and a bounce buffer alone does not substitute for it.
		 */
		.flags = {
			.fb_in_psram = true,
		},
	};
	memcpy(rgb_cfg.data_gpio_nums, cfg->pin_data, sizeof(rgb_cfg.data_gpio_nums));

	esp_lcd_rgb_timing_t timing = ST7701_480_480_PANEL_60HZ_RGB_TIMING();
	timing.h_res = cfg->width;
	timing.v_res = cfg->height;
	if (cfg->pclk_hz > 0) {
		timing.pclk_hz = cfg->pclk_hz;
	}

	/*
	 * Porches and sync widths come from the board rather than from the
	 * component's default macro when it supplies them.
	 *
	 * The default is generic for "an ST7701 at 480x480" and is not what every
	 * panel wired to one wants. The Waveshare ESP32-S3-Touch-LCD-4 needs a
	 * horizontal back porch of 50 against the macro's 10, and a panel given
	 * the wrong blanking does not sync: it stays dark while every call up the
	 * stack still succeeds, because an RGB panel's draw_bitmap only copies
	 * into a framebuffer and cannot tell whether the glass is showing it.
	 * Nothing reports an error, which is what makes this worth spelling out.
	 */
	if (cfg->hsync_pulse_width > 0) {
		timing.hsync_pulse_width = cfg->hsync_pulse_width;
		timing.hsync_back_porch = cfg->hsync_back_porch;
		timing.hsync_front_porch = cfg->hsync_front_porch;
		timing.vsync_pulse_width = cfg->vsync_pulse_width;
		timing.vsync_back_porch = cfg->vsync_back_porch;
		timing.vsync_front_porch = cfg->vsync_front_porch;
	}
	rgb_cfg.timings = timing;

	st7701_vendor_config_t vendor_cfg = {
		.init_cmds = cfg->init_cmds,
		.init_cmds_size = cfg->init_cmds_size,
		.rgb_config = &rgb_cfg,
		.flags = {
			.use_mipi_interface = 0,
			.auto_del_panel_io = 0,
			.mirror_by_cmd = 1,
		},
	};

	esp_lcd_panel_dev_config_t dev_cfg = {
		.reset_gpio_num = cfg->pin_rst,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
		.vendor_config = &vendor_cfg,
	};

	if (esp_lcd_new_panel_st7701(m_io, &dev_cfg, &m_panel) != ESP_OK) {
		ESP_LOGE(TAG, "panel create failed");
		disp_st7701_rgb_deinit();
		return false;
	}
	if (esp_lcd_panel_reset(m_panel) != ESP_OK ||
			esp_lcd_panel_init(m_panel) != ESP_OK) {
		ESP_LOGE(TAG, "panel init failed");
		disp_st7701_rgb_deinit();
		return false;
	}
	esp_lcd_panel_disp_on_off(m_panel, true);

	// Scratch space for format conversion and rotation. Two copies of a
	// chunk are needed when rotating.
	m_pix_buf_bytes = (size_t)cfg->width * CHUNK_LINES * 2U * 2U;
	/*
	 * 64-byte aligned, because the panel is configured with
	 * psram_trans_align = 64 and this buffer is a source for its transfers.
	 * heap_caps_malloc gives no such guarantee.
	 */
	m_pix_buf_bytes = (m_pix_buf_bytes + 63U) & ~(size_t)63U;
	m_pix_buf = heap_caps_aligned_alloc(64, m_pix_buf_bytes,
			MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!m_pix_buf) {
		m_pix_buf = heap_caps_aligned_alloc(64, m_pix_buf_bytes, MALLOC_CAP_8BIT);
	}
	if (!m_pix_buf) {
		ESP_LOGE(TAG, "pixel buffer alloc failed (%u bytes)", (unsigned)m_pix_buf_bytes);
		m_pix_buf_bytes = 0;
		disp_st7701_rgb_deinit();
		return false;
	}

#if defined(SCRIPT_ENGINE_LISP)
	lbm_add_extension("ext-disp-orientation", ext_disp_orientation);
#endif /* SCRIPT_ENGINE_LISP */

	return true;
}

bool disp_st7701_rgb_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
	if (!m_io) {
		return false;
	}
	return esp_lcd_panel_io_tx_param(m_io, cmd, data, len) == ESP_OK;
}

void disp_st7701_rgb_deinit(void) {
	if (m_panel) {
		esp_lcd_panel_del(m_panel);
		m_panel = NULL;
	}
	if (m_io) {
		esp_lcd_panel_io_del(m_io);
		m_io = NULL;
	}
	if (m_pix_buf) {
		heap_caps_free(m_pix_buf);
		m_pix_buf = NULL;
		m_pix_buf_bytes = 0;
	}
}

#endif
