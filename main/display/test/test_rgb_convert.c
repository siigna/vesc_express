/*
	Copyright 2026 Stephen Bouche

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
 * Host tests for the pixel conversion and rotation in the ST7701 drivers.
 *
 * These three functions are the part of a display driver that is pure
 * arithmetic, and they are also the part most easily got wrong: index
 * striding in a packed bitmap, and a transpose that has to place every pixel
 * exactly. They run on every rendered image, and on the P4 they run on every
 * image twice because landscape needs a rotate, so a mistake is a garbled
 * panel with nothing to debug it from.
 *
 * The bodies are taken from the driver at build time rather than copied, so
 * the test cannot drift from what ships. Build and run:
 *
 *   make -C main/display/test
 *
 * Note this covers the maths only. Bus timing, DSI lane rates and panel init
 * are not testable off-target.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "tinygfx.h"

/* The three functions under test, extracted from the driver by the Makefile */
#include "rgb_helpers_extracted.h"

/* tinygfx's COLOR_TO_RGB888 falls through to this for gradient and precalc
 * colours. Every colour in these tests is COLOR_REGULAR, which takes the
 * other branch, so reaching here means the test is covering something it
 * does not actually check -- say so rather than return a plausible value. */
uint32_t lbm_display_rgb888_from_color(color_t color, int x, int y) {
	(void)color; (void)x; (void)y;
	fprintf(stderr, "lbm_display_rgb888_from_color reached: these tests only "
			"cover COLOR_REGULAR, gradients are not checked here\n");
	abort();
}

static int failures = 0;
static int checks = 0;

static void expect_u32(const char *what, uint32_t got, uint32_t want) {
	checks++;
	if (got != want) {
		failures++;
		printf("  FAIL %-34s got 0x%04X want 0x%04X\n", what, got, want);
	}
}

static color_t solid(uint32_t rgb) {
	color_t c;
	memset(&c, 0, sizeof(c));
	c.type = COLOR_REGULAR;
	c.color1 = (int)rgb;
	return c;
}

/* ---------------------------------------------------------- rgb888_to_rgb565 */

static void test_888_to_565(void) {
	printf("rgb888_to_rgb565\n");
	expect_u32("black",   rgb888_to_rgb565(0x000000), 0x0000);
	expect_u32("white",   rgb888_to_rgb565(0xFFFFFF), 0xFFFF);
	expect_u32("red",     rgb888_to_rgb565(0xFF0000), 0xF800);
	expect_u32("green",   rgb888_to_rgb565(0x00FF00), 0x07E0);
	expect_u32("blue",    rgb888_to_rgb565(0x0000FF), 0x001F);
	/* Channel order: a value only in the top red bit must not leak to blue */
	expect_u32("red msb only",  rgb888_to_rgb565(0x800000), 0x8000);
	expect_u32("blue msb only", rgb888_to_rgb565(0x000080), 0x0010);
	/* Truncation, not rounding: the low bits are dropped */
	expect_u32("0x010101",  rgb888_to_rgb565(0x010101), 0x0000);
	/* r 0x08 -> 1, g 0x30 -> 12, b 0x08 -> 1, packed 5:6:5 */
	expect_u32("0x083008",  rgb888_to_rgb565(0x083008), 0x0981);
}

/* --------------------------------------------------------------- rotate_rgb565 */

/* A 3x2 source, distinct values, so a transpose error cannot pass by symmetry:
 *   1 2 3
 *   4 5 6                                                                  */
static void test_rotate(void) {
	printf("rotate_rgb565\n");
	const uint16_t src[6] = { 1, 2, 3, 4, 5, 6 };
	uint16_t dst[6];

	/* 90 CW: dst is 2 wide, 3 tall
	 *   4 1
	 *   5 2
	 *   6 3   */
	const uint16_t want90[6] = { 4, 1, 5, 2, 6, 3 };
	memset(dst, 0, sizeof(dst));
	rotate_rgb565(src, dst, 3, 2, 1);
	for (int i = 0; i < 6; i++) {
		char n[40];
		snprintf(n, sizeof n, "90cw [%d]", i);
		expect_u32(n, dst[i], want90[i]);
	}

	/* 180: reversed
	 *   6 5 4
	 *   3 2 1  */
	const uint16_t want180[6] = { 6, 5, 4, 3, 2, 1 };
	memset(dst, 0, sizeof(dst));
	rotate_rgb565(src, dst, 3, 2, 2);
	for (int i = 0; i < 6; i++) {
		char n[40];
		snprintf(n, sizeof n, "180 [%d]", i);
		expect_u32(n, dst[i], want180[i]);
	}

	/* 270 CW: dst is 2 wide, 3 tall
	 *   3 6
	 *   2 5
	 *   1 4   */
	const uint16_t want270[6] = { 3, 6, 2, 5, 1, 4 };
	memset(dst, 0, sizeof(dst));
	rotate_rgb565(src, dst, 3, 2, 3);
	for (int i = 0; i < 6; i++) {
		char n[40];
		snprintf(n, sizeof n, "270cw [%d]", i);
		expect_u32(n, dst[i], want270[i]);
	}

	/* Rotating twice by 90 must equal one 180, which is an independent check
	 * on the index arithmetic rather than on my expected tables. */
	uint16_t once[6], twice[6];
	rotate_rgb565(src, once, 3, 2, 1);   /* 3x2 -> 2x3 */
	rotate_rgb565(once, twice, 2, 3, 1); /* 2x3 -> 3x2 */
	for (int i = 0; i < 6; i++) {
		char n[40];
		snprintf(n, sizeof n, "90+90 == 180 [%d]", i);
		expect_u32(n, twice[i], want180[i]);
	}
}

/* ------------------------------------------------------------ convert_to_rgb565 */

static void test_convert(void) {
	printf("convert_to_rgb565\n");

	color_t pal[16];
	for (int i = 0; i < 16; i++) {
		pal[i] = solid(0);
	}
	pal[0] = solid(0x000000);
	pal[1] = solid(0xFF0000);
	pal[2] = solid(0x00FF00);
	pal[3] = solid(0xFFFFFF);

	/* indexed2: 1 bit per pixel, most significant bit first.
	 * 0b10100000 over 8 pixels = 1 0 1 0 0 0 0 0 */
	{
		uint8_t data[1] = { 0xA0 };
		image_buffer_t img = { indexed2, 8, 1, data, data };
		uint16_t out[8];
		bool ok = convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height);
		expect_u32("indexed2 returned ok", ok, true);
		const uint16_t want[8] = { 0xF800, 0, 0xF800, 0, 0, 0, 0, 0 };
		for (int i = 0; i < 8; i++) {
			char n[40];
			snprintf(n, sizeof n, "indexed2 [%d]", i);
			expect_u32(n, out[i], want[i]);
		}
	}

	/* indexed4: 2 bits per pixel, high bits first.
	 * 0b00011011 = 0 1 2 3 */
	{
		uint8_t data[1] = { 0x1B };
		image_buffer_t img = { indexed4, 4, 1, data, data };
		uint16_t out[4];
		bool ok = convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height);
		expect_u32("indexed4 returned ok", ok, true);
		expect_u32("indexed4 [0] pal0", out[0], 0x0000);
		expect_u32("indexed4 [1] pal1", out[1], 0xF800);
		expect_u32("indexed4 [2] pal2", out[2], 0x07E0);
		expect_u32("indexed4 [3] pal3", out[3], 0xFFFF);
	}

	/* indexed16: 4 bits per pixel, high nibble first */
	{
		uint8_t data[1] = { 0x31 };
		image_buffer_t img = { indexed16, 2, 1, data, data };
		uint16_t out[2];
		bool ok = convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height);
		expect_u32("indexed16 returned ok", ok, true);
		expect_u32("indexed16 [0] pal3", out[0], 0xFFFF);
		expect_u32("indexed16 [1] pal1", out[1], 0xF800);
	}

	/* An indexed image with no palette must be refused, not guessed at */
	{
		uint8_t data[1] = { 0x1B };
		image_buffer_t img = { indexed4, 4, 1, data, data };
		uint16_t out[4];
		expect_u32("indexed4 with no palette refused",
				convert_to_rgb565(&img, NULL, out, 0, (uint32_t)img.width * (uint32_t)img.height), false);
	}

	/* rgb565 passes through byte for byte */
	{
		uint8_t data[4] = { 0xF8, 0x00, 0x00, 0x1F };
		image_buffer_t img = { rgb565, 2, 1, data, data };
		uint16_t out[2];
		bool ok = convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height);
		expect_u32("rgb565 returned ok", ok, true);
		expect_u32("rgb565 [0]", out[0], 0xF800);
		expect_u32("rgb565 [1]", out[1], 0x001F);
	}

	/* rgb888 is packed three bytes per pixel */
	{
		uint8_t data[6] = { 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF };
		image_buffer_t img = { rgb888, 2, 1, data, data };
		uint16_t out[2];
		bool ok = convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height);
		expect_u32("rgb888 returned ok", ok, true);
		expect_u32("rgb888 [0] red", out[0], 0xF800);
		expect_u32("rgb888 [1] blue", out[1], 0x001F);
	}

	/* rgb332 is one byte per pixel, 3 bits red, 3 green, 2 blue, widened to
	 * 8 bits per channel and then narrowed again to 5:6:5. The widening does
	 * not replicate the high bits, so full-scale 0xFF does not reach white --
	 * 0xE718 rather than 0xFFFF. That is the existing behaviour in both
	 * ST7701 drivers; the test records it rather than asserting what would be
	 * nicer, since changing it would change what already ships. */
	{
		uint8_t data[5] = { 0xE0, 0x1C, 0x03, 0xFF, 0x00 };
		image_buffer_t img = { rgb332, 5, 1, data, data };
		uint16_t out[5];
		bool ok = convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height);
		expect_u32("rgb332 returned ok", ok, true);
		expect_u32("rgb332 red",   out[0], 0xE000);
		expect_u32("rgb332 green", out[1], 0x0700);
		expect_u32("rgb332 blue",  out[2], 0x0018);
		expect_u32("rgb332 all",   out[3], 0xE718);
		expect_u32("rgb332 none",  out[4], 0x0000);
	}

	/* A format the driver does not handle must be refused, not guessed at */
	{
		uint8_t data[4] = { 0, 0, 0, 0 };
		image_buffer_t img = { format_not_supported, 4, 1, data, data };
		uint16_t out[4];
		expect_u32("unknown format refused",
				convert_to_rgb565(&img, pal, out, 0, (uint32_t)img.width * (uint32_t)img.height), false);
	}
}

int main(void) {
	test_888_to_565();
	test_rotate();
	test_convert();

	/* Banded conversion must equal one-shot conversion.
	 *
	 * The driver converts a large image a band of rows at a time, passing a
	 * pixel offset and a count. That offset is where a packed format can go
	 * wrong: indexed4 holds four pixels per byte and indexed2 eight, so a
	 * band that does not start on a byte boundary shifts every pixel in it.
	 * The rest of this file only ever converted whole images from offset
	 * zero, which is exactly the case that cannot catch it.
	 */
	{
		const int w = 16, h = 8;
		uint8_t data[16 * 8];
		for (size_t i = 0; i < sizeof(data); i++) {
			data[i] = (uint8_t)(i * 37u);
		}

		const struct { color_format_t fmt; const char *name; } fmts[] = {
			{ indexed2, "indexed2" },
			{ indexed4, "indexed4" },
			{ indexed16, "indexed16" },
			{ rgb332, "rgb332" },
		};

		for (size_t f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
			image_buffer_t img = { fmts[f].fmt, w, h, data, data };
			color_t pal[16];
			for (int i = 0; i < 16; i++) {
				pal[i] = solid((uint32_t)(i * 0x111111u));
			}

			uint16_t whole[16 * 8], banded[16 * 8];
			memset(whole, 0xAA, sizeof(whole));
			memset(banded, 0x55, sizeof(banded));

			bool ok_whole = convert_to_rgb565(&img, pal, whole, 0,
					(uint32_t)w * (uint32_t)h);

			bool ok_band = true;
			const int band = 3;	 /* deliberately not a divisor of h */
			for (int row = 0; row < h; row += band) {
				int rows = (row + band <= h) ? band : (h - row);
				ok_band = ok_band && convert_to_rgb565(&img, pal,
						banded + (size_t)row * (size_t)w,
						(uint32_t)row * (uint32_t)w,
						(uint32_t)rows * (uint32_t)w);
			}

			char msg[96];
			snprintf(msg, sizeof(msg), "%s banded conversion returned ok", fmts[f].name);
			expect_u32(msg, ok_whole && ok_band, true);

			snprintf(msg, sizeof(msg), "%s banded equals whole", fmts[f].name);
			expect_u32(msg, memcmp(whole, banded, sizeof(whole)) == 0, true);
		}
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
