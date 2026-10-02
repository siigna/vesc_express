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
 *   (touch-load-gt911 TOUCH_SDA TOUCH_SCL TOUCH_RST TOUCH_INT 480 800)
 *
 * Note the touch size is the panel's NATIVE 480x800, not the rotated 800x480,
 * and the axes then need swapping with a mirror on one of them. Measured on
 * the board: native bounds, swap_xy, mirror_y. esp_lcd_touch computes
 * mirroring and clamping in the controller's own frame before any swap, so
 * handing it the rotated size makes the arithmetic right only where the error
 * cancels -- which presents as touch that is correct in the middle of the
 * screen and inverted towards the edges, and is easy to mistake for a
 * controller that needs calibrating.
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

/*
 * The 32 MB part leaves 23 MB past everything the firmware needs, so this
 * board gets the internal filesystem: f-connect-storage and f-storage-info,
 * mounting the storage partition on demand. Nothing is mounted at boot, so a
 * script that never asks pays nothing for this.
 *
 * The partition sits above the 16 MB line, which is reachable here but is a
 * property of the flash chip rather than of the board: esp_partition_read and
 * write go through esp_flash, which clamps the usable size to 16 MB unless
 * the chip driver claims SPI_FLASH_CHIP_CAP_32MB_SUPPORT. The GD25Q256 fitted
 * here (id 0xc84019) satisfies spi_flash_chip_gd_get_caps, whose test is
 * (chip_id & 0xFF) >= 0x19, and CONFIG_SPI_FLASH_SUPPORT_GD_CHIP is on. A
 * board built with a different 32 MB part should confirm that before trusting
 * the upper half -- the clamp is a warning at boot, not an error at the call.
 *
 * None of that applies to the memory-mapped path. flash_helper reads the
 * script and qml partitions through esp_partition_mmap, and those stay below
 * the line: see the header of partition_ota_32mb.csv.
 */
#define HW_INTERNAL_FS

/*
 * GPIO37/38 carry the CAN transceiver on this board and are also UART0.
 *
 * Normally that means no console UART and logs go out over USB-Serial/JTAG.
 * On this board, though, USB-Serial/JTAG is not exposed at all: the port
 * marked "USB" is the P4's USB 2.0 OTG, which only enumerates if the running
 * firmware implements a USB device class, and the port marked "USB to UART"
 * is a CH343 bridge -- wired, inevitably, to those same UART0 pads. Nothing
 * appeared on a host with either the factory firmware or ours, on either
 * port, which is what ruled USB-Serial/JTAG out: the ROM would have
 * enumerated it regardless of firmware.
 *
 * So a board with no dash package loaded has no way to be reached. Defining
 * BRINGUP_UART moves comms and the console onto UART0 through that bridge,
 * which is the only channel this board actually has:
 *
 *   idf.py -DHW_NAME="WS P4 Touch LCD 4.3" -DBRINGUP_UART=1 \
 *       -DEXTRA_SDKCONFIG=sdkconfig.bringup_uart build
 *
 * It deliberately also leaves CAN_TX_GPIO_NUM undefined, which stops main.c
 * starting CAN. Two peripherals cannot drive one pad, and the alternative is
 * the TWAI controller and a UART fighting over GPIO37/38 -- with a
 * transceiver attached, that means the console talking onto the vehicle bus.
 *
 * This is a bring-up configuration. Do not put it on a vehicle.
 */
/*
 * Two bring-up modes, because one UART cannot carry both and the difference
 * matters while diagnosing:
 *
 *   BRINGUP_UART      VESC comms on UART0. Console off. Lets a script be
 *                     uploaded, run and inspected, but a panic is silent.
 *   BRINGUP_CONSOLE   Console on UART0, no comms. Shows the whole boot and
 *                     any panic, which is the only way to tell "not
 *                     answering" from "crashed before it could".
 *
 * Both leave CAN undefined. On this board the transceiver is on GPIO37/38 --
 * the UART0 pads -- so CAN and either mode would be two peripherals driving
 * one pin pair.
 */
#if defined(BRINGUP_UART)
/*
 * UART_NUM has to be defined here, and not because this board needs a
 * non-default port. hwconf/hw.h reads
 *
 *     #ifndef UART_NUM
 *     #define HW_NO_UART
 *     #define UART_NUM 0
 *     ...
 *
 * so a board that sets HW_UART_COMM, UART_TX and UART_RX but leaves UART_NUM
 * alone gets HW_NO_UART defined for it as a side effect -- and main.c then
 * skips comm_uart_init entirely. The result is a board that boots normally,
 * logs normally, and never answers a packet, with nothing anywhere saying
 * why. Defining UART_NUM is what suppresses that block.
 */
#define HW_UART_COMM
#define UART_NUM                0
/*
 * 115200 by default, because that is what VESC Tool's board-setup path asks
 * for and what every existing script upload assumes. Override it from the
 * build when the link is the bottleneck:
 *
 *   idf.py -DCOMM_UART_BAUD=921600 ...
 *
 * The motivation is upload time: the dash package is 217 KB, which is a
 * quarter of a minute at 115200. Anything talking to a board built this way
 * has to agree on the rate, so tools/vesc_script.py probes rather than being
 * told.
 *
 * Measured on this board with the 217 KB dash package: 31 s at 115200 and
 * 12 s at 921600, both including the reset, the erase and starting the
 * script. 921600 is verified working end to end.
 *
 * An earlier version of this comment said the opposite -- that a board built
 * for a higher rate answered nothing at all. That was a poisoned build
 * directory whose first configure had failed, not the baud: the same source
 * built in a working directory answers fine, and the control that would have
 * caught it was building 115200 in the broken directory, which is silent too.
 */
#ifndef COMM_UART_BAUD
#define COMM_UART_BAUD          115200
#endif
#define UART_BAUDRATE           COMM_UART_BAUD
#define UART_TX                 37
#define UART_RX                 38
#else
#define HW_NO_UART
#endif

#define HW_INIT_HOOK()          hw_init()

/* Deliberately NOT using HW_EARLY_LBM_INIT. It moves lispif_init ahead of
 * comm_can_start and log_mount_card (main.c:123 vs :171), and this board
 * wants both: the dash reads telemetry over CAN and logs to the SD slot.
 * The reason other displays use it -- getting something on screen before the
 * radios come up -- is handled here by parking the backlight in hw_init
 * instead, which still runs before lispif_init on the default path. */

// CAN (onboard transceiver)
#if !defined(BRINGUP_UART) && !defined(BRINGUP_CONSOLE)
#define CAN_TX_GPIO_NUM         37
#define CAN_RX_GPIO_NUM         38
#endif

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
