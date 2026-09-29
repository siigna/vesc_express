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
 * Layout for LispBM screen descriptions, on top of Clay.
 *
 * A screen is data, not code:
 *
 *   (clay-layout
 *     '(col (pad 8) (gap 6)
 *        (row (h 54) (gap 6)
 *          (box (w fit) (h 40) (pad 10) (bg 1) (radius 6)
 *            (text (font 1) (str . "LIGHT") (fg 3)))))
 *
 * Element props: w h (a number, grow, fit, or (pct f)), pad / padx / pady,
 * gap, bg, radius, alignx (left / center / right),
 * aligny (top / center / bottom).
 * Text props: font, size, fg, str.
 *     480 480
 *     (list font-16 font-24))
 *
 * where (font N) indexes the font list passed in, and the result is a flat
 * list of drawing commands in back-to-front order:
 *
 *   ((rect x y w h bg radius) (text x y w h fg font str) ...)
 *
 * The font travels in the command because the drawer needs it, and it is the
 * caller's index rather than one this file invents, so both sides agree
 * without depending on the order elements happen to be walked in.
 *
 * which lisp draws with img-rectangle, ttf-text and disp-render -- the same
 * primitives the hand-written views already use, on whichever display driver
 * happens to be loaded. Clay never learns that a display exists, which is what
 * keeps this testable on a workstation.
 *
 * The point of the exercise is that the description carries no coordinates.
 * Clay sizes and positions everything, so one description fits a 480x480 and
 * an 800x480 panel, which a description written in absolute pixels cannot.
 *
 * Colours are palette indices rather than RGB, because the display layer draws
 * indexed images and resolves the palette at disp-render time.
 */

/* Clay costs about 80 kB of flash, and only a board with a display can use
 * it, so it is opt-in. HW_USE_CLAY is set by main/CMakeLists.txt for the
 * targets that have the room; everything else gets the stub at the bottom. */
#include "clay_layout.h"

#ifdef HW_USE_CLAY

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lispbm.h"
#include "extensions/ttf_extensions.h"

#define CLAY_IMPLEMENTATION
#include "clay/clay.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CLAY_ARENA_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CLAY_ARENA_ALLOC_FALLBACK(n) heap_caps_malloc((n), MALLOC_CAP_8BIT)
#else
#define CLAY_ARENA_ALLOC(n) malloc(n)
#define CLAY_ARENA_ALLOC_FALLBACK(n) NULL
#endif

/* A dash page is tens of elements. Clay's own default is 8192, which would
 * want megabytes; this lands the arena in the low hundreds of kB. */
#define CLAY_MAX_ELEMENTS   256
#define CLAY_MAX_CACHE_WORDS 256

/* The caller's font list, decoded once per call so the measure callback can
 * reach it by the index Clay carries in fontId. */
#define MAX_FONTS 16
typedef struct {
	const uint8_t *data;
	int32_t size;
} font_ref;

static font_ref m_fonts[MAX_FONTS];
static int m_font_num = 0;

static Clay_Context *m_ctx = NULL;
static void *m_arena_mem = NULL;

// Symbols, interned once
static lbm_uint sym_col, sym_row, sym_box, sym_text;
static lbm_uint sym_w, sym_h, sym_pad, sym_padx, sym_pady, sym_gap, sym_bg, sym_fg;
static lbm_uint sym_radius, sym_font, sym_size, sym_str, sym_align;
static lbm_uint sym_alignx, sym_aligny, sym_left, sym_center, sym_right, sym_top, sym_bottom;
static lbm_uint sym_grow, sym_fit, sym_pct;
static lbm_uint sym_rect, sym_border, sym_clip_start, sym_clip_end;
static bool m_syms_ok = false;

static bool intern_syms(void) {
	if (m_syms_ok) {
		return true;
	}
	bool ok = true;
	ok = ok && lbm_add_symbol_const("col", &sym_col);
	ok = ok && lbm_add_symbol_const("row", &sym_row);
	ok = ok && lbm_add_symbol_const("box", &sym_box);
	ok = ok && lbm_add_symbol_const("text", &sym_text);
	ok = ok && lbm_add_symbol_const("w", &sym_w);
	ok = ok && lbm_add_symbol_const("h", &sym_h);
	ok = ok && lbm_add_symbol_const("pad", &sym_pad);
	ok = ok && lbm_add_symbol_const("padx", &sym_padx);
	ok = ok && lbm_add_symbol_const("pady", &sym_pady);
	ok = ok && lbm_add_symbol_const("gap", &sym_gap);
	ok = ok && lbm_add_symbol_const("bg", &sym_bg);
	ok = ok && lbm_add_symbol_const("fg", &sym_fg);
	ok = ok && lbm_add_symbol_const("radius", &sym_radius);
	ok = ok && lbm_add_symbol_const("font", &sym_font);
	ok = ok && lbm_add_symbol_const("size", &sym_size);
	ok = ok && lbm_add_symbol_const("str", &sym_str);
	ok = ok && lbm_add_symbol_const("align", &sym_align);
	ok = ok && lbm_add_symbol_const("alignx", &sym_alignx);
	ok = ok && lbm_add_symbol_const("aligny", &sym_aligny);
	ok = ok && lbm_add_symbol_const("left", &sym_left);
	ok = ok && lbm_add_symbol_const("center", &sym_center);
	ok = ok && lbm_add_symbol_const("right", &sym_right);
	ok = ok && lbm_add_symbol_const("top", &sym_top);
	ok = ok && lbm_add_symbol_const("bottom", &sym_bottom);
	ok = ok && lbm_add_symbol_const("grow", &sym_grow);
	ok = ok && lbm_add_symbol_const("fit", &sym_fit);
	ok = ok && lbm_add_symbol_const("pct", &sym_pct);
	ok = ok && lbm_add_symbol_const("rect", &sym_rect);
	ok = ok && lbm_add_symbol_const("border", &sym_border);
	ok = ok && lbm_add_symbol_const("clip-start", &sym_clip_start);
	ok = ok && lbm_add_symbol_const("clip-end", &sym_clip_end);
	m_syms_ok = ok;
	return ok;
}

/* ---------------------------------------------------------------- measuring */

static Clay_Dimensions measure_cb(Clay_StringSlice text,
		Clay_TextElementConfig *config, void *userData) {
	(void)userData;

	if (config->fontId >= m_font_num) {
		return (Clay_Dimensions){ 0, 0 };
	}
	const font_ref *f = &m_fonts[config->fontId];

	/* lbm_ttf_measure wants a C string, and a Clay slice is not terminated:
	 * Clay hands out sub-ranges of the original when it wraps text. */
	char buf[256];
	int32_t n = text.length;
	if (n > (int32_t)sizeof(buf) - 1) {
		n = (int32_t)sizeof(buf) - 1;
	}
	memcpy(buf, text.chars, (size_t)n);
	buf[n] = 0;

	float w = 0;
	float h = 0;
	if (!lbm_ttf_measure(f->data, f->size, buf, 1.0f, &w, &h)) {
		return (Clay_Dimensions){ 0, 0 };
	}
	return (Clay_Dimensions){ w, h };
}

/* ------------------------------------------------------------------ decoding */

// Property lookup: a property is (name value) or (name . value)
static bool prop_get(lbm_value el, lbm_uint name, lbm_value *out) {
	lbm_value curr = lbm_cdr(el);
	while (lbm_is_cons(curr)) {
		lbm_value item = lbm_car(curr);
		if (lbm_is_cons(item) && lbm_is_symbol(lbm_car(item))) {
			lbm_uint s = lbm_dec_sym(lbm_car(item));
			if (s == name) {
				lbm_value v = lbm_cdr(item);
				// (name value) gives a one-element list, (name . value) the value
				*out = lbm_is_cons(v) ? lbm_car(v) : v;
				return true;
			}
		}
		curr = lbm_cdr(curr);
	}
	return false;
}

static float prop_num(lbm_value el, lbm_uint name, float dflt) {
	lbm_value v;
	if (prop_get(el, name, &v) && lbm_is_number(v)) {
		return lbm_dec_as_float(v);
	}
	return dflt;
}

static bool is_kind(lbm_value v) {
	if (!lbm_is_cons(v) || !lbm_is_symbol(lbm_car(v))) {
		return false;
	}
	lbm_uint s = lbm_dec_sym(lbm_car(v));
	return s == sym_col || s == sym_row || s == sym_box || s == sym_text;
}

/* w and h accept a number for a fixed size, the symbols grow or fit, or
 * (pct f) for a fraction of the parent -- which is what a bar fill wants, so
 * that a battery gauge is a percentage rather than a computed width.
 * The default is fit, which is Clay's own default. */
static Clay_SizingAxis size_axis(lbm_value el, lbm_uint name) {
	lbm_value v;
	if (prop_get(el, name, &v)) {
		if (lbm_is_number(v)) {
			return CLAY_SIZING_FIXED(lbm_dec_as_float(v));
		}
		if (lbm_is_symbol(v)) {
			if (lbm_dec_sym(v) == sym_grow) {
				return CLAY_SIZING_GROW(0);
			}
		} else if (lbm_is_cons(v) && lbm_is_symbol(lbm_car(v)) &&
				lbm_dec_sym(lbm_car(v)) == sym_pct &&
				lbm_is_cons(lbm_cdr(v)) && lbm_is_number(lbm_car(lbm_cdr(v)))) {
			float f = lbm_dec_as_float(lbm_car(lbm_cdr(v)));
			/* Clay rejects a percentage above 1 with an error rather than
			 * clamping, and a dash will hand it a ratio from live data. */
			if (f < 0.0f) {
				f = 0.0f;
			}
			if (f > 1.0f) {
				f = 1.0f;
			}
			return CLAY_SIZING_PERCENT(f);
		}
	}
	return CLAY_SIZING_FIT(0);
}

/* How children are placed inside an element. Defaults to centred on both
 * axes, which is what a pill or a value box wants; a label column wants its
 * text pushed to one edge, and that is what alignx is for. */
static Clay_ChildAlignment child_align(lbm_value el) {
	Clay_ChildAlignment a = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
	lbm_value v;
	if (prop_get(el, sym_alignx, &v) && lbm_is_symbol(v)) {
		lbm_uint s = lbm_dec_sym(v);
		if (s == sym_left) {
			a.x = CLAY_ALIGN_X_LEFT;
		} else if (s == sym_right) {
			a.x = CLAY_ALIGN_X_RIGHT;
		}
	}
	if (prop_get(el, sym_aligny, &v) && lbm_is_symbol(v)) {
		lbm_uint s = lbm_dec_sym(v);
		if (s == sym_top) {
			a.y = CLAY_ALIGN_Y_TOP;
		} else if (s == sym_bottom) {
			a.y = CLAY_ALIGN_Y_BOTTOM;
		}
	}
	return a;
}

/* Palette indices travel in the alpha-less part of a Clay_Color. Clay only
 * copies colours through to the render commands, so smuggling an index in the
 * red channel is safe and avoids resolving a palette here, where the display
 * format is not known. */
static Clay_Color pal_index(float i) {
	return (Clay_Color){ i, 0, 0, 255 };
}

// (font N), an index into the font list the caller passed in
static int font_id_for(lbm_value el) {
	lbm_value v;
	if (!prop_get(el, sym_font, &v) || !lbm_is_number(v)) {
		return -1;
	}
	int i = lbm_dec_as_i32(v);
	if (i < 0 || i >= m_font_num) {
		return -1;
	}
	return i;
}

/* Decode the caller's list of prepared fonts. The arrays stay owned by lisp;
 * we only borrow the pointers, and they are reachable from the argument for
 * the whole call. */
static bool fonts_load(lbm_value list) {
	m_font_num = 0;
	lbm_value curr = list;
	while (lbm_is_cons(curr)) {
		if (m_font_num >= MAX_FONTS) {
			return false;
		}
		lbm_value f = lbm_car(curr);
		if (!lbm_is_array_r(f)) {
			return false;
		}
		lbm_array_header_t *arr = lbm_dec_array_r(f);
		if (!arr || arr->size < 10) {
			return false;
		}
		m_fonts[m_font_num].data = (const uint8_t *)arr->data;
		m_fonts[m_font_num].size = (int32_t)arr->size;
		m_font_num++;
		curr = lbm_cdr(curr);
	}
	return m_font_num > 0;
}

/* ------------------------------------------------------------------- walking */

static void walk(lbm_value el);

static void walk_children(lbm_value el) {
	lbm_value curr = lbm_cdr(el);
	while (lbm_is_cons(curr)) {
		lbm_value item = lbm_car(curr);
		if (is_kind(item)) {
			walk(item);
		}
		curr = lbm_cdr(curr);
	}
}

static void walk(lbm_value el) {
	lbm_uint kind = lbm_dec_sym(lbm_car(el));

	if (kind == sym_text) {
		lbm_value sv;
		if (!prop_get(el, sym_str, &sv) || !lbm_is_array_r(sv)) {
			return;
		}
		lbm_array_header_t *sa = lbm_dec_array_r(sv);
		if (!sa) {
			return;
		}
		int fid = font_id_for(el);
		if (fid < 0) {
			return;
		}

		/* The array carries a trailing zero; Clay wants the length without it */
		int32_t len = (int32_t)sa->size;
		while (len > 0 && ((const char *)sa->data)[len - 1] == 0) {
			len--;
		}

		Clay_String s = { false, len, (const char *)sa->data };
		float align = prop_num(el, sym_align, 0);
		Clay__OpenTextElement(s, (Clay_TextElementConfig){
			.textColor = pal_index(prop_num(el, sym_fg, 3)),
			.fontId = (uint16_t)fid,
			.fontSize = (uint16_t)prop_num(el, sym_size, 16),
			.textAlignment = align > 1.5f ? CLAY_TEXT_ALIGN_RIGHT
			               : align > 0.5f ? CLAY_TEXT_ALIGN_CENTER
			                              : CLAY_TEXT_ALIGN_LEFT,
			.wrapMode = CLAY_TEXT_WRAP_NONE,
		});
		return;
	}

	/* pad sets all four edges; padx and pady override one axis, which is what
	 * a strip wants when it needs an inset from the sides without becoming
	 * taller than the text it holds. */
	float pad_all = prop_num(el, sym_pad, 0);
	uint16_t pad_x = (uint16_t)prop_num(el, sym_padx, pad_all);
	uint16_t pad_y = (uint16_t)prop_num(el, sym_pady, pad_all);
	lbm_value bgv;
	bool has_bg = prop_get(el, sym_bg, &bgv) && lbm_is_number(bgv);

	Clay__OpenElement();
	Clay__ConfigureOpenElement((Clay_ElementDeclaration){
		.layout = {
			.sizing = { size_axis(el, sym_w), size_axis(el, sym_h) },
			.padding = { pad_x, pad_x, pad_y, pad_y },
			.childGap = (uint16_t)prop_num(el, sym_gap, 0),
			.childAlignment = child_align(el),
			.layoutDirection = kind == sym_col ? CLAY_TOP_TO_BOTTOM
			                                   : CLAY_LEFT_TO_RIGHT,
		},
		/* An element with no bg emits no rectangle, which is what we want for
		 * pure layout containers. Clay treats alpha 0 as nothing to draw. */
		.backgroundColor = has_bg ? pal_index(lbm_dec_as_float(bgv))
		                          : (Clay_Color){ 0, 0, 0, 0 },
		.cornerRadius = CLAY_CORNER_RADIUS(prop_num(el, sym_radius, 0)),
	});

	walk_children(el);
	Clay__CloseElement();
}

/* ------------------------------------------------------- command list output */

static lbm_value cmd_list(Clay_RenderCommandArray cmds) {
	lbm_value res = ENC_SYM_NIL;

	/* Built back to front so the returned list is in Clay's draw order */
	for (int32_t i = cmds.length - 1; i >= 0; i--) {
		Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&cmds, i);
		Clay_BoundingBox b = c->boundingBox;
		lbm_value item = ENC_SYM_NIL;

		switch (c->commandType) {
		case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
			item = lbm_heap_allocate_list_init(7,
					lbm_enc_sym(sym_rect),
					lbm_enc_i((lbm_int)b.x), lbm_enc_i((lbm_int)b.y),
					lbm_enc_i((lbm_int)b.width), lbm_enc_i((lbm_int)b.height),
					lbm_enc_i((lbm_int)c->renderData.rectangle.backgroundColor.r),
					lbm_enc_i((lbm_int)c->renderData.rectangle.cornerRadius.topLeft));
			if (lbm_is_symbol(item)) {
				return item;
			}
			break;
		}
		case CLAY_RENDER_COMMAND_TYPE_TEXT: {
			Clay_StringSlice s = c->renderData.text.stringContents;
			lbm_value str;
			if (!lbm_create_array(&str, (lbm_uint)s.length + 1)) {
				return ENC_SYM_MERROR;
			}
			lbm_array_header_t *sa = (lbm_array_header_t *)lbm_car(str);
			memcpy(sa->data, s.chars, (size_t)s.length);
			((char *)sa->data)[s.length] = 0;

			item = lbm_heap_allocate_list_init(8,
					lbm_enc_sym(sym_text),
					lbm_enc_i((lbm_int)b.x), lbm_enc_i((lbm_int)b.y),
					lbm_enc_i((lbm_int)b.width), lbm_enc_i((lbm_int)b.height),
					lbm_enc_i((lbm_int)c->renderData.text.textColor.r),
					lbm_enc_i((lbm_int)c->renderData.text.fontId),
					str);
			if (lbm_is_symbol(item)) {
				return item;
			}
			break;
		}
		case CLAY_RENDER_COMMAND_TYPE_BORDER: {
			item = lbm_heap_allocate_list_init(7,
					lbm_enc_sym(sym_border),
					lbm_enc_i((lbm_int)b.x), lbm_enc_i((lbm_int)b.y),
					lbm_enc_i((lbm_int)b.width), lbm_enc_i((lbm_int)b.height),
					lbm_enc_i((lbm_int)c->renderData.border.color.r),
					lbm_enc_i((lbm_int)c->renderData.border.width.left));
			if (lbm_is_symbol(item)) {
				return item;
			}
			break;
		}
		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
			bool start = c->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_START;
			item = lbm_heap_allocate_list_init(5,
					lbm_enc_sym(start ? sym_clip_start : sym_clip_end),
					lbm_enc_i((lbm_int)b.x), lbm_enc_i((lbm_int)b.y),
					lbm_enc_i((lbm_int)b.width), lbm_enc_i((lbm_int)b.height));
			if (lbm_is_symbol(item)) {
				return item;
			}
			break;
		}
		default:
			continue;
		}

		lbm_value cell = lbm_cons(item, res);
		if (lbm_is_symbol(cell)) {
			return cell;
		}
		res = cell;
	}
	return res;
}

/* ----------------------------------------------------------------- extension */

static void clay_error(Clay_ErrorData e) {
	/* Clay reports these rather than aborting; surfacing them is better than
	 * silently drawing a wrong screen. */
	lbm_set_error_reason("Clay layout error");
	(void)e;
}

static bool clay_ensure_init(float w, float h) {
	if (m_ctx) {
		Clay_SetCurrentContext(m_ctx);
		Clay_SetLayoutDimensions((Clay_Dimensions){ w, h });
		return true;
	}

	Clay_SetMaxElementCount(CLAY_MAX_ELEMENTS);
	Clay_SetMaxMeasureTextCacheWordCount(CLAY_MAX_CACHE_WORDS);

	uint32_t need = Clay_MinMemorySize();
	m_arena_mem = CLAY_ARENA_ALLOC(need);
	if (!m_arena_mem) {
		m_arena_mem = CLAY_ARENA_ALLOC_FALLBACK(need);
	}
	if (!m_arena_mem) {
		return false;
	}

	Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(need, m_arena_mem);
	m_ctx = Clay_Initialize(arena, (Clay_Dimensions){ w, h },
			(Clay_ErrorHandler){ clay_error, 0 });
	if (!m_ctx) {
		free(m_arena_mem);
		m_arena_mem = NULL;
		return false;
	}
	Clay_SetMeasureTextFunction(measure_cb, NULL);
	return true;
}

static lbm_value ext_clay_layout(lbm_value *args, lbm_uint argn) {
	if (argn != 4 || !lbm_is_cons(args[0]) ||
			!lbm_is_number(args[1]) || !lbm_is_number(args[2]) ||
			!lbm_is_cons(args[3])) {
		return ENC_SYM_TERROR;
	}
	if (!intern_syms()) {
		return ENC_SYM_EERROR;
	}
	if (!is_kind(args[0])) {
		lbm_set_error_reason("Expected a layout element");
		return ENC_SYM_EERROR;
	}

	float w = lbm_dec_as_float(args[1]);
	float h = lbm_dec_as_float(args[2]);
	if (w <= 0 || h <= 0) {
		return ENC_SYM_TERROR;
	}
	if (!clay_ensure_init(w, h)) {
		lbm_set_error_reason("Could not allocate the Clay arena");
		return ENC_SYM_EERROR;
	}

	/* Font pointers are borrowed from arrays the caller holds, so they are
	 * live for the whole call. Nothing between here and the end of the layout
	 * allocates on the LBM heap, so GC cannot run while Clay holds them. */
	if (!fonts_load(args[3])) {
		lbm_set_error_reason("Expected a list of prepared fonts");
		return ENC_SYM_EERROR;
	}

	Clay_BeginLayout();
	walk(args[0]);
	Clay_RenderCommandArray cmds = Clay_EndLayout(0.0f);

	return cmd_list(cmds);
}

void lbm_clay_extensions_init(void) {
	lbm_add_extension("clay-layout", ext_clay_layout);
}

#else

void lbm_clay_extensions_init(void) {
}

#endif
