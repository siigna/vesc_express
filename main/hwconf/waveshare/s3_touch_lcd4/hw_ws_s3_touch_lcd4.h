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
 * an onboard CAN transceiver.
 *
 * HW_TARGET gets its own sdkconfig rather than reusing the N16R8 one, even
 * though it is the same module, because an RGB panel here has to coexist with
 * flash writes. That needs XIP from PSRAM; see the header of
 * sdkconfig.defaults.esp32s3_ws_lcd4.
 *
 * Pin map taken from Waveshare's own board support, cross-checked against
 * payalneg/Super_VESC_DIsplay which runs on this board.
 */

#ifndef HW_WS_S3_TOUCH_LCD4_H_
#define HW_WS_S3_TOUCH_LCD4_H_

#define HW_NAME                 "WS S3 Touch LCD 4"
#define HW_TARGET               "esp32s3_ws_lcd4"
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
/*
 * 16 MHz, the ST7701 component's figure, which Waveshare's ESP-IDF BSP also
 * keeps. Their Arduino driver picks 12 MHz for octal PSRAM instead; 12 MHz was
 * tried here and changed nothing that could be seen, so this stays at the
 * value both the component and the BSP use.
 */
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
/*
 * Blanking from Waveshare's Arduino example for this board, which passes
 * these explicitly rather than taking the ST7701 component's generic macro.
 *
 * The macro's own values are 10/10/20 and 10/10/10, and their ESP-IDF BSP
 * uses the macro unchanged. Both were tried on the board. These are the ones
 * that were in place when the panel produced correct colour, so these stay,
 * and the disagreement between the two vendor references is noted rather than
 * resolved.
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

/* EXIO0..EXIO7, from the pin table on the V4.0 schematic.
 *
 * Not from the Waveshare wiki, whose EXIO numbering is for the pre-V4 boards
 * that used a TCA9554, and not only from the vendor's header, which names bits
 * 1, 3, 5, 6 and 7 but leaves 2 and 4 unnamed. Those two matter.
 */
#define CH32_PIN_TP_RST			(1 << 1)
#define CH32_PIN_TP_INT			(1 << 2)	// selects the GT911 address
#define CH32_PIN_LCD_RST		(1 << 3)
#define CH32_PIN_SDCS			(1 << 4)	// SD card chip select, idles high
#define CH32_PIN_SYS_EN			(1 << 5)
#define CH32_PIN_BEE_EN			(1 << 6)	// buzzer: high sounds it
#define CH32_PIN_RTC_INT		(1 << 7)	// the RTC drives this

/*
 * Measured power-on state, with the panel lit and everything working:
 *
 *     DIRECTION 0xFF   OUTPUT 0xFF   INPUT 0x00   PWM 0x00
 *
 * That is the reference. The only things this firmware needs to change are a
 * reset pulse on TP_RST and LCD_RST, so it starts from all-high and drops just
 * those two, rather than from the vendor's OUT_DISPLAY_ON of 0x2A.
 *
 * 0x2A sets bits 1, 3 and 5 and clears everything else, which also drives
 * TP_INT low -- moving the GT911 from 0x14 to 0x5D and holding down the line
 * the touch driver wants as an interrupt input -- asserts SDCS, and fights the
 * RTC's own output on RTC_INT. Three peripherals disturbed to reset two.
 *
 * BEE_EN is held low rather than left at the 0xFF the board idles with.
 * Writing this register with bit 6 set sounds the buzzer and nothing short of
 * a power cycle stops it: the CH32 has no register that silences it, so a
 * write of 0xFF here is not recoverable in software. Every value below clears
 * bit 6 for that reason, and it is the one deliberate departure from the
 * measured idle state.
 */
/* The board idles with this, and it is what the vendor writes too. Left as
 * measured rather than narrowed: the semantics of this register are not
 * deducible from the schematic, and every attempt to reason about them here
 * has cost something. */
#define CH32_DIR_DEFAULT		0xFF

#define CH32_OUT_RUN			(0xFF & ~CH32_PIN_BEE_EN)
#define CH32_OUT_RESET			(CH32_OUT_RUN & ~(CH32_PIN_TP_RST | CH32_PIN_LCD_RST))

/*
 * Backlight, and it is not an enable.
 *
 * EXIO_PWM is its own CH32 pin, and on the schematic it feeds the feedback
 * divider of the AP3032KTR-G1 boost LED driver through a 10K resistor and a
 * 1uF filter, alongside the 62K from the 5.1 ohm current sense. Raising its
 * average voltage raises the feedback node, the driver reads that as too much
 * LED current and backs off. So the duty is inverted: 0 is full brightness and
 * 255 is off, which the topology explains rather than merely asserts.
 *
 * Writing 255 here is what kept this panel dark, on every boot, while the rest
 * of the driver reported success.
 */
#define CH32_PWM_BRIGHTEST		0

// Touch (GT911, I2C). Reset is on the CH32, not on a pin of its own.
#define TOUCH_SDA               15
#define TOUCH_SCL               7
#define TOUCH_RST               (-1)	// see CH32_PIN_TOUCH_RST
#define TOUCH_INT               16

// Functions
void hw_init(void);

#endif /* HW_WS_S3_TOUCH_LCD4_H_ */
