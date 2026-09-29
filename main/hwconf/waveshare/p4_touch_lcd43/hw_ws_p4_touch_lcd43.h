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
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
 *
 * ESP32-P4 with 32 MB PSRAM, a 4.3" 480x800 ST7701 panel on 2-lane MIPI-DSI,
 * GT911 touch, an onboard CAN transceiver, an SD slot and an ESP32-C6 bridge
 * for WiFi.
 *
 * The panel is already supported: disp_st7701.c drives exactly this
 * controller over DSI, so there is nothing to add in C. A package brings the
 * display up with
 *
 *   (disp-load-st7701 DISP_RST DISP_LANE_MBPS)
 *   (ext-disp-orientation 1)                 ; 480x800 native -> 800x480
 *   (touch-load-gt911 TOUCH_SDA TOUCH_SCL TOUCH_RST TOUCH_INT 800 480)
 *
 * and the backlight with pwm-start on DISP_BACKLIGHT. That pin is active-LOW,
 * so the duty has to be inverted: (pwm-set-duty (- 1.0 level) chan).
 *
 * NOTE: rotation is a software transpose in disp_st7701.c, costing two
 * heap allocations and a CPU rotate per rendered image. That is affordable
 * while rendering small regions, and is why the dash redraws only the fields
 * that changed.
 *
 * HW_TARGET gets its own sdkconfig rather than reusing the P4 NRW32N16 one,
 * because that config puts the console on UART0 -- which on this chip is the
 * same pair of pads the CAN transceiver uses. See the console block in
 * sdkconfig.defaults.esp32p4_ws_lcd43.
 */

#ifndef HW_WS_P4_TOUCH_LCD43_H_
#define HW_WS_P4_TOUCH_LCD43_H_

#define HW_NAME                 "WS P4 Touch LCD 4.3"
#define HW_TARGET               "esp32p4_ws_lcd43"

/* GPIO37/38 carry the CAN transceiver on this board and are also UART0, so
 * there is no console UART. Logs go out over USB-Serial/JTAG. */
#define HW_NO_UART

#define HW_INIT_HOOK()          hw_init()

/* Deliberately NOT using HW_EARLY_LBM_INIT. It moves lispif_init ahead of
 * comm_can_start and log_mount_card (main.c:123 vs :171), and this board
 * wants both: the dash reads telemetry over CAN and logs to the SD slot.
 * The reason other displays use it -- getting something on screen before the
 * radios come up -- is handled here by parking the backlight in hw_init
 * instead, which still runs before lispif_init on the default path. */

// CAN (onboard transceiver)
#define CAN_TX_GPIO_NUM         37
#define CAN_RX_GPIO_NUM         38

// Display. Panel reset, and the DSI lane rate disp-load-st7701 is given.
#define DISP_WIDTH              800     // after (ext-disp-orientation 1)
#define DISP_HEIGHT             480
#define DISP_RST                27
#define DISP_LANE_MBPS          500
#define DISP_BACKLIGHT          26
#define DISP_BACKLIGHT_ACTIVE_LOW 1
#define DISP_BACKLIGHT_OFF_LEVEL  1     // active-LOW, so 1 is off

// Touch (GT911, I2C). No touch INT is broken out to the P4.
#define TOUCH_SDA               7
#define TOUCH_SCL               8
#define TOUCH_RST               23
#define TOUCH_INT               (-1)

// SD card (SPI; log.c has no SDMMC path)
#define SD_PIN_MOSI             44      // SD CMD
#define SD_PIN_MISO             39      // SD D0
#define SD_PIN_SCK              43      // SD CLK
#define SD_PIN_CS               42      // SD D3

/* ADC. hw.h would default these to ADC1 channels 0-3, which on this board are
 * GPIO16-19 — the C6 bridge. Only channels 4-6 are free, so CH3 and CH4 are
 * aliased rather than left to the default; lispif_vesc_extensions.c reads all
 * five unconditionally. Do not wire anything to the aliased channels. */
#define HW_ADC_CH0              ADC1_CHANNEL_4  // GPIO20
#define HW_ADC_CH1              ADC1_CHANNEL_5  // GPIO21
#define HW_ADC_CH2              ADC1_CHANNEL_6  // GPIO22
#define HW_ADC_CH3              HW_ADC_CH0      // no free pin
#define HW_ADC_CH4              HW_ADC_CH0      // no free pin

// Functions
void hw_init(void);

#endif /* HW_WS_P4_TOUCH_LCD43_H_ */
