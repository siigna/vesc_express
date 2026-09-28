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
 * Waveshare ESP32-S3-Touch-LCD-4
 *
 * ESP32-S3-N16R8, 4" 480x480 ST7701 over a parallel RGB bus, GT911 touch, and
 * an onboard CAN transceiver. HW_TARGET reuses the N16R8 sdkconfig because it
 * is the same module.
 *
 * Pin map taken from Waveshare's own board support, cross-checked against
 * payalneg/Super_VESC_DIsplay which runs on this board.
 */

#ifndef HW_WS_S3_TOUCH_LCD4_H_
#define HW_WS_S3_TOUCH_LCD4_H_

#define HW_NAME                 "WS S3 Touch LCD 4"
#define HW_TARGET               "esp32s3_n16r8"
#define HW_UART_COMM

#define HW_INIT_HOOK()          hw_init()

// CAN (onboard transceiver)
#define CAN_TX_GPIO_NUM         6
#define CAN_RX_GPIO_NUM         0

// UART
#define UART_NUM                0
#define UART_BAUDRATE           115200
#define UART_TX                 43
#define UART_RX                 44

// Display: ST7701 register setup over a 3-wire software SPI link
#define DISP_CS                 42
#define DISP_SCLK               2
#define DISP_SDA                1
#define DISP_RST                (-1)	// Tied off on the board

// Display: parallel RGB bus
#define DISP_WIDTH              480
#define DISP_HEIGHT             480
#define DISP_PCLK_HZ            16000000
#define DISP_DE                 40
#define DISP_VSYNC              39
#define DISP_HSYNC              38
#define DISP_PCLK               41

// Data lines in bus order for a 16-bit RGB565 panel: B0..B4, G0..G5, R0..R4
#define DISP_DATA_PINS          {5, 45, 48, 47, 21, \
                                 14, 13, 12, 11, 10, 9, \
                                 46, 3, 8, 18, 17}

// Touch (GT911, I2C). Reset is not broken out, so it stays unasserted.
#define TOUCH_SDA               15
#define TOUCH_SCL               7
#define TOUCH_RST               (-1)
#define TOUCH_INT               16

// Functions
void hw_init(void);

#endif /* HW_WS_S3_TOUCH_LCD4_H_ */
