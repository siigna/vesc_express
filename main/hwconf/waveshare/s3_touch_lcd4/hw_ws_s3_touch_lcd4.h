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

/* Blanking, from Waveshare's own Arduino panel configuration for this board.
 *
 * The ST7701 component's generic 480x480 defaults are 10/10/20 horizontal and
 * 10/10/10 vertical, and this panel wants a horizontal back porch of 50. With
 * the wrong blanking it never syncs and shows nothing at all, while every call
 * in the driver still reports success -- a draw on an RGB panel only copies
 * into a framebuffer, so nothing in the stack can tell whether the glass is
 * displaying it.
 */
#define DISP_HSYNC_PULSE        8
#define DISP_HSYNC_BACK_PORCH   50
#define DISP_HSYNC_FRONT_PORCH  10
#define DISP_VSYNC_PULSE        8
#define DISP_VSYNC_BACK_PORCH   20
#define DISP_VSYNC_FRONT_PORCH  10
#define DISP_DE                 40
#define DISP_VSYNC              39
#define DISP_HSYNC              38
#define DISP_PCLK               41

// Data lines in bus order for a 16-bit RGB565 panel: B0..B4, G0..G5, R0..R4
#define DISP_DATA_PINS          {5, 45, 48, 47, 21, \
                                 14, 13, 12, 11, 10, 9, \
                                 46, 3, 8, 18, 17}

/* The CH32V003F4U6 helper, on the same I2C bus as the touch controller.
 *
 * This is the part of the board that is easy to miss, and nothing works
 * without it. On Rev4.0 the panel's reset, the touch reset, the panel's power
 * rail and the backlight are all behind this microcontroller rather than on
 * ESP32 pins -- so a display that never gets reset, a touch controller that
 * never appears on the bus, and a dark backlight are one fault, not three.
 * Waveshare's own notes put it plainly: "a CH32 or shared-bus failure can
 * appear as several unrelated peripheral failures".
 *
 * Earlier revisions of this board used a TCA9554 with a different pin order,
 * and the Waveshare wiki still documents that one. Its EXIO numbering is wrong
 * for Rev4.0 in every position that matters. These values come from the
 * vendor's WS_CH32_IO library and their ioexpander ESP-IDF example, which are
 * the only register-level references for this revision.
 *
 * Registers, and a direction bit set to 1 for an output:
 */
#define CH32_ADDR				0x24
#define CH32_REG_DIRECTION		0x02
#define CH32_REG_OUTPUT			0x03
#define CH32_REG_INPUT			0x04
#define CH32_REG_PWM			0x05
#define CH32_REG_ADC			0x06

#define CH32_PIN_TOUCH_RST		(1 << 1)
#define CH32_PIN_LCD_RST		(1 << 3)
#define CH32_PIN_SYS_EN			(1 << 5)
#define CH32_PIN_BEE_EN			(1 << 6)	// buzzer: never driven high here
#define CH32_PIN_RTC_INT		(1 << 7)	// input, left alone

// Bit 7 stays an input for the RTC. The buzzer is an output so that it is
// held low deliberately rather than left floating.
#define CH32_DIR_OUTPUTS		(CH32_PIN_TOUCH_RST | CH32_PIN_LCD_RST | \
								 CH32_PIN_SYS_EN | CH32_PIN_BEE_EN)

#define CH32_OUT_RESET			0x00
#define CH32_OUT_DISPLAY_ON		(CH32_PIN_SYS_EN | CH32_PIN_LCD_RST | \
								 CH32_PIN_TOUCH_RST)

// Touch (GT911, I2C). Reset is on the CH32, not on a pin of its own.
#define TOUCH_SDA               15
#define TOUCH_SCL               7
#define TOUCH_RST               (-1)	// see CH32_PIN_TOUCH_RST
#define TOUCH_INT               16

// Functions
void hw_init(void);

#endif /* HW_WS_S3_TOUCH_LCD4_H_ */
