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
 * Render a packed Lua script to an image, on the host.
 *
 *   render_host script.luapkg out.ppm [width height [args...]]
 *
 * Anything after the size becomes the global `arg`, a table indexed from 1,
 * the way the standalone interpreter presents its command line. That lets one
 * harness script render more than one board profile, and lets a script behave
 * the same here as it does under host Lua.
 *
 * The script may also call vesc.save_frame("path.ppm") to emit the
 * framebuffer as it stands, as many times as it likes. That is what lets one
 * script walk every page and save each, the way the lisp harness calls
 * save-active-img in a loop; out.ppm is still written at the end, so a
 * single-image case needs nothing new.
 *
 * This is the Lua counterpart of what the lispBM repl does for the lisp dash:
 * it runs the real drawing bindings against a framebuffer instead of a panel,
 * so a view can be checked against the goldens in dash_common/test without a
 * board. Porting fourteen hundred lines of view by eye is how the text index
 * bug got in; this is the thing that makes it unnecessary.
 *
 * Everything that draws is compiled from the firmware tree -- tinygfx, the
 * display bindings, the colour core, the font readers -- so what renders here
 * is what renders on the glass. The one piece that is not shared is the
 * driver below, which composites into memory rather than pushing to a panel.
 *
 * PPM out, not PNG. A PPM writer is nine lines and needs no dependency, and
 * the comparison against a PNG golden belongs in the script that already has
 * Python available rather than in a C encoder written for one caller.
 */

#include "../script_lua.h"
#include "../script_pack.h"
#include "../lua_vesc_ext.h"
#include "../../display/disp_backend.h"
#include "tinygfx.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fb_w = 800;
static int fb_h = 480;
static uint8_t *fb;	// RGB888, fb_w * fb_h * 3

static void fb_put(int x, int y, uint32_t rgb) {
	if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) {
		return;
	}
	uint8_t *p = fb + ((size_t)y * (size_t)fb_w + (size_t)x) * 3;
	p[0] = (rgb >> 16) & 0xFF;
	p[1] = (rgb >> 8) & 0xFF;
	p[2] = rgb & 0xFF;
}

/*
 * The driver. Composites an indexed buffer through its palette, which is the
 * one thing a real panel does that the fake driver in test_script_lua does
 * not: that one records the call and discards the pixels, because it is
 * testing the binding rather than the picture.
 */
static bool host_render(image_buffer_t *img, uint16_t x, uint16_t y,
		color_t *colors) {
	if (!img) {
		return false;
	}

	for (int yy = 0; yy < img->height; yy++) {
		for (int xx = 0; xx < img->width; xx++) {
			uint32_t v = getpixel(img, xx, yy);
			uint32_t rgb;

			if (colors) {
				// Through the same helper the panel drivers use, so a
				// gradient resolves here the way it does on hardware.
				rgb = lbm_display_rgb888_from_color(colors[v], xx, yy);
			} else {
				rgb = v;
			}

			fb_put(x + xx, y + yy, rgb);
		}
	}

	return true;
}

static void host_clear(uint32_t color) {
	for (int y = 0; y < fb_h; y++) {
		for (int x = 0; x < fb_w; x++) {
			fb_put(x, y, color);
		}
	}
}

static void host_reset(void) {
	host_clear(0);
}

static bool host_orientation(int rot) {
	(void)rot;
	// Accepted and ignored: the goldens are rendered at the panel's final
	// size, so a rotation here would turn the image twice.
	return true;
}

static void print_cb(const char *str) {
	printf("%s\n", str);
}

static bool never_stop(void) {
	return false;
}

static int write_ppm(const char *path);

/*
 * vesc.save_frame(path) -- the framebuffer as it stands, as a PPM.
 *
 * The counterpart of the lisp repl's save-active-img, and only here: nothing
 * on a board has a file to write to. Raises rather than returning a code,
 * because a harness that silently failed to write a golden would compare the
 * previous one and pass.
 */
static int l_save_frame(lua_State *L) {
	const char *path = luaL_checkstring(L, 1);
	if (write_ppm(path) != 0) {
		return luaL_error(L, "save_frame: cannot write %s", path);
	}
	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg host_funcs[] = {
	{"save_frame", l_save_frame},
	{NULL, NULL},
};

static int write_ppm(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", fb_w, fb_h);
	fwrite(fb, 1, (size_t)fb_w * (size_t)fb_h * 3, f);
	fclose(f);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s script.luapkg out.ppm [width height]\n", argv[0]);
		return 2;
	}

	if (argc >= 5) {
		fb_w = atoi(argv[3]);
		fb_h = atoi(argv[4]);
	}

	if (fb_w < 1 || fb_h < 1 || fb_w > 4096 || fb_h > 4096) {
		fprintf(stderr, "implausible size %dx%d\n", fb_w, fb_h);
		return 2;
	}

	fb = calloc((size_t)fb_w * (size_t)fb_h, 3);
	if (!fb) {
		fprintf(stderr, "out of memory for %dx%d\n", fb_w, fb_h);
		return 1;
	}

	FILE *in = fopen(argv[1], "rb");
	if (!in) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	static uint8_t blob[512 * 1024];
	size_t n = fread(blob, 1, sizeof(blob), in);
	fclose(in);

	script_blob_t parsed;
	if (!script_pack_parse(blob, (int32_t)n, &parsed)) {
		fprintf(stderr, "%s is not a valid container\n", argv[1]);
		return 1;
	}
	if (parsed.lang != SCRIPT_LANG_LUA) {
		fprintf(stderr, "%s is not a Lua container\n", argv[1]);
		return 1;
	}

	script_lua_cfg_t cfg = {
		.mem_limit = 8 * 1024 * 1024,	// generous: this is a desktop
		.print = print_cb,
		.should_stop = never_stop,
		.hook_count = 0,
		.blob = &parsed,
	};

	script_lua_t *s = script_lua_open(&cfg);
	if (!s) {
		fprintf(stderr, "could not create an interpreter\n");
		return 1;
	}

	// The drawing surface, registered the way a panel driver registers.
	disp_backend_set(host_render, host_clear, host_reset);
	disp_backend_set_orientation_fn(host_orientation);

	lua_vesc_disp_register(s);
	lua_vesc_color_register(s);
	lua_vesc_font_register(s);
	script_lua_install_events(s);
	script_lua_register(s, host_funcs);

	// The global `arg`, from whatever followed the size.
	if (argc > 5) {
		lua_State *L = script_lua_state(s);
		if (L) {
			lua_createtable(L, argc - 5, 0);
			for (int i = 5;i < argc;i++) {
				lua_pushstring(L, argv[i]);
				lua_rawseti(L, -2, i - 4);
			}
			lua_setglobal(L, "arg");
		}
	}

	char err[512] = {0};
	if (!script_lua_run(s, parsed.src, parsed.src_len, "=view", err, sizeof(err))) {
		fprintf(stderr, "script failed: %s\n", err);
		script_lua_close(s);
		return 1;
	}

	script_lua_close(s);
	return write_ppm(argv[2]);
}
