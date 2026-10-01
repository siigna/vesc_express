-- The Lua half of the renderer calibration. See tools/img_diff.py.
--
-- Draws exactly what calib/cal.lisp draws. The text base index is 1, which is
-- what a lisp colour list of (0 1 2 3) means: coverage 1..3 lands on palette
-- 1..3.
-- A known picture: a red rectangle, a green one, and white text, so the
-- framebuffer can be checked by value rather than by eye.
local font = vesc.font_load(vesc.asset("font18"))

vesc.disp_clear(0x000000)

local buf = vesc.img_buffer("indexed4", 200, 60)
buf:clear()
buf:rectangle(0, 0, 100, 60, 1, true)
buf:text(110, 40, font, "AB", 1, true)
vesc.disp_render(buf, 10, 10, {0x000000, 0xFF0000, 0x00FF00, 0xFFFFFF})

print("rendered")
