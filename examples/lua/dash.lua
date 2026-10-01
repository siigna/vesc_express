-- A dash page in Lua.
--
-- Pack it with the fonts it needs:
--
--   tools/luapack.py examples/lua/dash.lua -o dash.luapkg \
--       --asset font-big=font/roboto-bold-120-4c.bin \
--       --asset font-small=font/roboto-bold-24-4c.bin
--
-- Prepared fonts come from ttf-prepare; the ones in vesc_pkg/dash_p4/font
-- work as they are.
--
-- Two things shape the whole design. Renders are expensive -- a full 800x480
-- frame is 384000 pixels through a software transpose on this board -- so each
-- field owns a buffer sized to itself and only redraws when its text changes.
-- And every CAN reading is age-gated, because the getters return the last
-- value seen whether it arrived a millisecond or a minute ago.

local W, H = 800, 480

-- Palette. Entries 1..3 are a coverage ramp so text antialiases; the colours
-- the fields use sit above that.
local PAL = {
    0x000000,  -- 0 background
    0x303030,  -- 1 text, dim coverage
    0x909090,  -- 2 text, mid coverage
    0xFFFFFF,  -- 3 text, full
    0x00FF88,  -- 4 good
    0xFFCC00,  -- 5 warn
    0xFF3030,  -- 6 bad
    0x202020,  -- 7 panel
    0x606060,  -- 8 label
}
local BG, TEXT, GOOD, WARN, BAD, PANEL, LABEL = 0, 1, 4, 5, 6, 7, 8

local font_big = vesc.font_load(assert(vesc.asset("font-big"),
        "bundle font-big with --asset"))
local font_small = vesc.font_load(assert(vesc.asset("font-small"),
        "bundle font-small with --asset"))

vesc.gpio_configure(26, "out")
vesc.gpio_write(26, false)
assert(vesc.disp_load("st7701", 27, 500), "panel did not load")
vesc.disp_orientation(1)
vesc.disp_clear(0x000000)

local has_touch = pcall(function()
    -- Native panel bounds, then swap with one mirror. See doc/lua.md.
    vesc.touch_load_gt911(7, 8, 23, -1, 480, 800)
    vesc.touch_transform(true, false, true)
end)

-- Age gate. canget_* hands back the last value seen with no indication of
-- when, so a controller that dropped off the bus still reads plausibly and an
-- id that only ever reported zeros reads as a confident zero.
local MAX_AGE = 0.5

local function fresh(id, read)
    local age = vesc.can_msg_age(id)
    if not age or age > MAX_AGE then return nil end
    return read()
end

local ID = 0

local fields = {
    {label = "km/h", fmt = "%.0f",
     read = function()
        local v = fresh(ID, function() return vesc.canget_rpm(ID) end)
        return v and v / 100 or nil
     end,
     colour = function(v) return v and TEXT or LABEL end},

    {label = "volts", fmt = "%.1f",
     -- Falls back to the local ADC so the field shows something true rather
     -- than nothing when no controller is reporting.
     read = function()
        return fresh(ID, function() return vesc.canget_vin(ID) end)
            or vesc.get_adc(0)
     end,
     colour = function(v)
        if not v then return LABEL end
        return v < 3.0 and BAD or (v < 3.3 and WARN or GOOD)
     end},

    {label = "amps", fmt = "%.1f",
     read = function() return fresh(ID, function() return vesc.canget_current(ID) end) end,
     colour = function(v) return v and TEXT or LABEL end},

    {label = "deg C", fmt = "%.0f",
     read = function() return fresh(ID, function() return vesc.canget_temp_fet(ID) end) end,
     colour = function(v)
        if not v then return LABEL end
        return v > 80 and BAD or (v > 60 and WARN or GOOD)
     end},
}

-- Which field gets the large readout. A tap cycles it, so the target is the
-- whole screen and it works with gloves.
local big = 1

-- Geometry is computed rather than written down, so changing the field list
-- or the screen size does not mean editing coordinates.
local function layout()
    local small_h = 96
    for i, f in ipairs(fields) do
        if i == big then
            f.x, f.y, f.font, f.big = 40, 150, font_big, true
            f.w, f.h = 420, 150
        else
            local slot = 0
            for j = 1, #fields do
                if j ~= big then
                    slot = slot + 1
                    if j == i then break end
                end
            end
            f.x, f.y, f.font, f.big = 500, 60 + (slot - 1) * small_h, font_small, false
            f.w, f.h = 260, small_h - 8
        end
        f.buf = vesc.img_buffer("indexed4", f.w, f.h)
        f.last = nil
    end
end

local header = vesc.img_buffer("indexed4", W, 40)

local function draw_static()
    header:clear(PANEL)
    header:rectangle(0, 0, W, 4, GOOD, true)
    header:text(12, 30, font_small, "VESC", TEXT, true)
    vesc.disp_render(header, 0, 0, PAL)
end

local function redraw(f, v)
    local text = v and string.format(f.fmt, v) or "--"
    f.buf:clear(BG)
    -- Baseline placed so the digits sit on it with the label underneath for
    -- the large field, beside it for the small ones.
    if f.big then
        f.buf:text(0, 120, f.font, text, f.colour(v), true)
        f.buf:text(4, 148, font_small, f.label, LABEL, true)
    else
        f.buf:text(0, 60, f.font, text, f.colour(v), true)
        f.buf:text(0, 84, font_small, f.label, LABEL, true)
    end
    vesc.disp_render(f.buf, f.x, f.y, PAL)
end

local function full_redraw()
    vesc.disp_clear(0x000000)
    draw_static()
    for _, f in ipairs(fields) do redraw(f, nil) end
end

layout()
full_redraw()
print("dash up, touch", has_touch)

local was_down = false
local frames = 0

vesc.on_timer(100, function()
    frames = frames + 1

    if has_touch then
        local tx = vesc.touch_read()
        if tx and not was_down then
            big = big % #fields + 1
            layout()
            full_redraw()
        end
        was_down = tx ~= nil
    end

    for _, f in ipairs(fields) do
        local v = f.read()
        -- Compared on the formatted text, not the value: a reading that
        -- wobbles in a decimal place that is not displayed must not cost a
        -- redraw every frame.
        local text = v and string.format(f.fmt, v) or "--"
        if text ~= f.last then
            f.last = text
            redraw(f, v)
        end
    end

    if frames % 100 == 0 then
        print("frames", frames, "dropped events", vesc.events_dropped())
    end
end)
