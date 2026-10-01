-- A dash page in Lua.
--
-- Draws into one indexed16 buffer and renders only the regions that changed,
-- because a full 800x480 render is 384000 pixels through a software transpose
-- on this board -- far too slow for a 10 Hz update. The layout is a table, so
-- adding a field is data rather than code.

local PAL = {
    0x000000,  -- 1 background
    0x202020,  -- 2 panel
    0x00FF88,  -- 3 good
    0xFFCC00,  -- 4 warn
    0xFF3030,  -- 5 bad
    0xFFFFFF,  -- 6 text
    0x808080,  -- 7 dim
}
local BG, PANEL, GOOD, WARN, BAD, TEXT, DIM = 0, 1, 2, 3, 4, 5, 6

local W, H = 800, 480

-- Seven-segment digits, drawn with rectangles: no font support is bound yet,
-- and a dash is mostly numbers. Segments are a,b,c,d,e,f,g.
local SEG = {
    ["0"] = "abcdef",  ["1"] = "bc",     ["2"] = "abdeg",  ["3"] = "abcdg",
    ["4"] = "bcfg",    ["5"] = "acdfg",  ["6"] = "acdefg", ["7"] = "abc",
    ["8"] = "abcdefg", ["9"] = "abcdfg", ["-"] = "g",      ["."] = "",
    [" "] = "",
}

local function digit(buf, x, y, w, h, t, ch, colour)
    local segs = SEG[ch]
    if not segs then return end
    local function on(s) return segs:find(s, 1, true) ~= nil end
    if on("a") then buf:rectangle(x + t, y, w - 2*t, t, colour, true) end
    if on("b") then buf:rectangle(x + w - t, y + t, t, h/2 - t, colour, true) end
    if on("c") then buf:rectangle(x + w - t, y + h/2, t, h/2 - t, colour, true) end
    if on("d") then buf:rectangle(x + t, y + h - t, w - 2*t, t, colour, true) end
    if on("e") then buf:rectangle(x, y + h/2, t, h/2 - t, colour, true) end
    if on("f") then buf:rectangle(x, y + t, t, h/2 - t, colour, true) end
    if on("g") then buf:rectangle(x + t, y + h/2 - t/2, w - 2*t, t, colour, true) end
    if ch == "." then buf:rectangle(x, y + h - t, t, t, colour, true) end
end

local function number(buf, x, y, w, h, t, str, colour)
    local cx = x
    for i = 1, #str do
        local ch = str:sub(i, i)
        local cw = (ch == "." ) and t * 2 or w
        digit(buf, cx, y, cw, h, t, ch, colour)
        cx = cx + cw + t
    end
    return cx - x
end

-- Age gate for CAN readings.
--
-- canget_* returns the last value seen, with no indication of when it
-- arrived: a controller that dropped off the bus a minute ago still reads
-- plausibly, and an id that has only ever reported zeros reads as a confident
-- zero. The first run of this dash on hardware showed 0 V because of exactly
-- that. So every reading goes through here, and anything older than the
-- limit is treated as absent.
local MAX_AGE = 0.5

local function fresh(id, read)
    local age = vesc.can_msg_age(id)
    if not age or age > MAX_AGE then
        return nil
    end
    return read()
end

-- Fields: where they are, how to get a value, and how to colour it. Adding a
-- reading means adding a row here.
local fields = {
    {name = "speed", x = 40,  y = 60,  w = 54, h = 110, t = 12,
     read = function()
        local v = fresh(0, function() return vesc.canget_rpm(0) end)
        return v and v / 100 or nil
     end,
     fmt = "%.0f", colour = function(v) return v and TEXT or DIM end},
    {name = "volts", x = 420, y = 60,  w = 34, h = 70, t = 8,
     -- Falls back to the local ADC when no controller is reporting, so the
     -- field shows something true rather than nothing.
     read = function()
        return fresh(0, function() return vesc.canget_vin(0) end) or vesc.get_adc(0)
     end,
     fmt = "%.1f",
     colour = function(v) if not v then return DIM end
        return v < 3.0 and BAD or (v < 3.3 and WARN or GOOD) end},
    {name = "amps",  x = 420, y = 190, w = 34, h = 70, t = 8,
     read = function() return fresh(0, function() return vesc.canget_current(0) end) end,
     fmt = "%.1f", colour = function(v) return v and TEXT or DIM end},
    {name = "temp",  x = 420, y = 320, w = 34, h = 70, t = 8,
     read = function() return fresh(0, function() return vesc.canget_temp_fet(0) end) end,
     fmt = "%.0f",
     colour = function(v) if not v then return DIM end
        return v > 80 and BAD or (v > 60 and WARN or GOOD) end},
}

vesc.gpio_configure(26, "out")
vesc.gpio_write(26, false)
assert(vesc.disp_load("st7701", 27, 500), "panel did not load")
vesc.disp_orientation(1)
vesc.disp_clear(0x000000)

-- Touch, if this board has it. Native panel bounds and then swap plus one
-- mirror; see doc/lua.md for why the rotated size does not work here.
local has_touch = pcall(function()
    vesc.touch_load_gt911(7, 8, 23, -1, 480, 800)
    vesc.touch_transform(true, false, true)
end)
print("touch", has_touch)

-- One buffer per field, sized to that field, so a changed reading costs a
-- render of its own box rather than of the screen.
for _, f in ipairs(fields) do
    f.buf = vesc.img_buffer("indexed16", f.w * 6 + f.t * 6, f.h)
    f.last = nil
end

-- Static furniture, drawn once.
local frame = vesc.img_buffer("indexed16", W, 40)
frame:clear(PANEL)
frame:rectangle(0, 0, W, 3, GOOD, true)
vesc.disp_render(frame, 0, 0, PAL)

local function redraw(f, v)
    local text = v and string.format(f.fmt, v) or "---"
    f.buf:clear(BG)
    number(f.buf, 0, 0, f.w, f.h, f.t, text, f.colour(v))
    vesc.disp_render(f.buf, f.x, f.y, PAL)
end

for _, f in ipairs(fields) do redraw(f, nil) end
print("dash up")

-- Which reading gets the large readout. Tapping cycles it, which is the
-- whole interaction: a glove-friendly target is the whole screen.
local big = 1

local function relayout()
    -- The large field swaps place with whichever was large before, so the
    -- geometry stays fixed and only the contents move.
    for i, f in ipairs(fields) do
        f.last = nil
        if i == big then
            f.x, f.y, f.w, f.h, f.t = 40, 60, 54, 110, 12
        else
            f.x, f.y, f.w, f.h, f.t = 420, 60 + (i - 1) * 130, 34, 70, 8
        end
        f.buf = vesc.img_buffer("indexed16", f.w * 6 + f.t * 6, f.h)
    end
    vesc.disp_clear(0x000000)
    vesc.disp_render(frame, 0, 0, PAL)
end

local was_down = false
local frames = 0
vesc.on_timer(100, function()
    frames = frames + 1

    if has_touch then
        local tx = vesc.touch_read()
        if tx and not was_down then
            big = big % #fields + 1
            relayout()
            print("large field is now", fields[big].name)
        end
        was_down = tx ~= nil
    end
    local drawn = 0
    for _, f in ipairs(fields) do
        local v = f.read()
        -- Compare on the formatted string: a current reading that wobbles in
        -- the third decimal must not cost a redraw every frame.
        local text = v and string.format(f.fmt, v) or "---"
        if text ~= f.last then
            f.last = text
            redraw(f, v)
            drawn = drawn + 1
        end
    end
    if frames % 50 == 0 then
        print("frames", frames, "redrawn this frame", drawn,
              "dropped events", vesc.events_dropped())
    end
end)
print("timer running")
