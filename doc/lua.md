# Lua scripting

This firmware can be built with either of two script engines. The default is
LispBM, unchanged. Building with `-DSCRIPT_ENGINE=lua` replaces it with Lua
5.4:

```
idf.py -DSCRIPT_ENGINE=lua -DHW_NAME="S3 N16R8" build
```

One engine or the other, never both: they occupy the same budget, and on
STM32 there is only room for one. The two interpreter cores are within 1% of
each other in flash — 86,974 B for LispBM against 87,456 B for Lua on
Cortex-M4 — so the choice costs nothing either way.

## Writing and uploading a script

Pack the script, then upload the container with any existing tool:

```
tools/luapack.py main.lua -o main.luapkg
```

`luapack.py` follows `require` calls and bundles each module into the
container, transitively. The container format is the one VESC Tool has always
written for lisp, so `--uploadLisp`, the package builder and the REPL all
carry Lua unchanged. Bit 0 of the previously unused flags word records which
language the source is, which is what lets one package store hold both.

A `require` whose argument is not a literal string cannot be bundled —
`luapack.py` reports it rather than guessing, and it will fail at runtime.

## Talking to a board without VESC Tool

`tools/vesc_script.py` uploads, runs and watches scripts over serial:

```
tools/vesc_script.py /dev/ttyACM0 ping
tools/vesc_script.py /dev/ttyACM0 upload main.luapkg
tools/vesc_script.py /dev/ttyACM0 stats
tools/vesc_script.py /dev/ttyACM0 console -s 6     # raw log, console builds
```

It handles the parts that are easy to get wrong, and each of them cost real
time to find:

- **Opening the port can reset the board.** DTR and RTS drive BOOT and EN on a
  USB-UART bridge, so a tool that sends a command shortly after opening is
  talking to a chip still in its bootloader — indistinguishable from a board
  that has stopped answering.
- **Two frame formats.** Short frames lead with `0x02` and one length byte,
  long frames with `0x03` and two. A parser handling only the short form
  misses long replies silently, which is enough to conclude the engine is not
  running when it is.
- **Script output is not stored.** It arrives within milliseconds of the run
  command, so anything waiting for a specific reply has to keep the output it
  passes over instead of discarding it.
- **Output goes to the last port that received a packet.** A script printing
  at boot, before anything has connected, prints into nowhere. For that, use a
  console build — script output is mirrored to the console there.

## What is available

Lua's base, `coroutine`, `string`, `table` and `math` libraries, and a `vesc`
table holding everything else.

Deliberately absent: `io` and `os` (filesystem and clock), `package` (loads
native libraries), `debug` (reaches around every other restriction), and
`dofile`, `loadfile`, `load` and `collectgarbage`. `load` would let a script
compile a string at runtime, defeating any review of what was actually
flashed; `collectgarbage` would let it switch the collector off on a device
where the memory ceiling is the only thing keeping a script away from the
comms stack.

### Naming

A lisp extension `can-send-sid` is `vesc.can_send_sid`. The rule is
mechanical, and `tools/script_ext_coverage.py` uses it to report which lisp
extensions have bindings yet:

```
tools/script_ext_coverage.py --missing
```

### System

| function | notes |
|---|---|
| `vesc.systime()` | tick count |
| `vesc.secs_since(t)` | seconds since a tick count |
| `vesc.sleep(secs)` | yields; never spin-waits |
| `vesc.get_adc([ch])` | volts |
| `vesc.events_dropped()` | events the queue could not deliver |

### GPIO

| function | notes |
|---|---|
| `vesc.gpio_configure(pin, mode)` | mode: `in`, `in-pu`, `in-pd`, `out` |
| `vesc.gpio_write(pin, bool)` | |
| `vesc.gpio_read(pin)` | |

### CAN

Payloads are strings, not tables. Use `string.pack` and `string.unpack` to
build and read them — a string is already a counted byte buffer, and it
allocates nothing per byte in a handler running at a few hundred hertz.

```lua
vesc.can_send_sid(0x123, string.pack("<i2i2", 1000, -500))
```

| function | notes |
|---|---|
| `vesc.can_send_sid(id, data)` | at most 8 bytes |
| `vesc.can_send_eid(id, data)` | |
| `vesc.can_ping(id)` | returns ok, hw_type |
| `vesc.can_list_devs()` | table of ids seen in the last 2 s |
| `vesc.canget_current(id)` | |
| `vesc.canget_current_dir(id)` | signed by direction |
| `vesc.canget_current_in(id)` | |
| `vesc.canget_duty(id)` | |
| `vesc.canget_rpm(id)` | |
| `vesc.canget_temp_fet(id)` | |
| `vesc.canget_temp_motor(id)` | |
| `vesc.canget_vin(id)` | |
| `vesc.canget_ppm(id)` | |
| `vesc.canget_adc(id, [ch])` | ch 1..3 |
| `vesc.canget_tacho(id)`, `vesc.canget_dist(id)` | |
| `vesc.can_msg_age(id, [msg])` | seconds; msg 1..6 |
| `vesc.canset_current(id, a, [off_delay])` | |
| `vesc.canset_current_rel(id, rel, [off_delay])` | |
| `vesc.canset_duty(id, duty)` | |
| `vesc.canset_brake(id, a)` | |
| `vesc.canset_brake_rel(id, rel)` | |
| `vesc.canset_rpm(id, rpm)` | |
| `vesc.canset_pos(id, deg)` | |
| `vesc.canset_handbrake(id, a)` | |
| `vesc.canset_handbrake_rel(id, rel)` | |

**Every `canget_*` returns `nil` when that id has not reported**, rather than
zero. A controller that is off, unplugged or not yet seen is a different thing
from one reporting zero current, and treating them alike is how a dash ends up
showing a confident `0 A` for a motor that is not there. The getters also
return the *last* value seen with no indication of age, so check
`vesc.can_msg_age(id)` before trusting them:

```lua
local age = vesc.can_msg_age(5)
if age and age < 0.5 then
  print(vesc.canget_rpm(5))
end
```

### eeprom

512 slots, the same numbering the lisp engine uses, so a package ported
between engines keeps its stored settings.

| function | notes |
|---|---|
| `vesc.eeprom_store_i(addr, int)` | |
| `vesc.eeprom_store_f(addr, num)` | |
| `vesc.eeprom_read_i(addr)` | `nil` if never written |
| `vesc.eeprom_read_f(addr)` | `nil` if never written |
| `vesc.eeprom_erase()` | all slots |

An unwritten slot reads `nil`, not zero. This bit a lisp dash package once:
the test stub returned zero, the hardware returned nothing, and the package
treated the zero as a real setting. Guard every read.

### UART

| function | notes |
|---|---|
| `vesc.uart_start(port, rx, tx, baud)` | re-starting changes baud |
| `vesc.uart_stop()` | |
| `vesc.uart_write(str)` | returns bytes written |
| `vesc.uart_read([max], [timeout_ms])` | returns a string, empty on timeout |
| `vesc.uart_available()` | bytes buffered |

### I2C

Port 0, the same bus the board's own drivers use.

| function | notes |
|---|---|
| `vesc.i2c_start(sda, scl, [hz])` | default 200 kHz |
| `vesc.i2c_stop()` | |
| `vesc.i2c_tx_rx(addr, [tx], [rx_len])` | see below |
| `vesc.i2c_detect_addr(addr)` | |

`i2c_tx_rx` covers all three shapes a device needs. A write followed by a read
happens in one transaction with no stop in between, which is how nearly every
register read works:

```lua
local v = vesc.i2c_tx_rx(0x68, string.char(0x3B), 6)   -- write reg, read 6
vesc.i2c_tx_rx(0x68, string.char(0x6B, 0x00))          -- write only
local raw = vesc.i2c_tx_rx(0x68, nil, 2)               -- read only
```

It returns the bytes read, or `true` for a write with no read, or `nil` plus
an error string on a bus error — so a missing device is distinguishable from
one that answered with zeros.

### Bringing a panel up

```lua
vesc.disp_load("st7789", 6, 5, 7, 8, 4, 40)   -- sd0, clk, cs, reset, dc, mhz
vesc.disp_load("ssd1306", 7, 6, 700000)       -- sda, scl, hz
```

`vesc.disp_panels()` returns what this build can load. One binding rather than
one per panel, because almost every SPI panel in the tree takes the same six
arguments:

| panels | arguments |
|---|---|
| `st7789`, `st7789a`, `ili9341`, `ili9488`, `gc9a01`, `jd9853`, `sh8601`, `ssd1351`, `st7735` | `sd0, clk, cs, reset, dc, [mhz]` |
| `sh8501b`, `icna3306` | `sd0, clk, cs, reset, [mhz]` |
| `ssd1306` | `sda, scl, [hz]` |
| `st7701` | `rst, [lane_mbps]` — MIPI-DSI, ESP32-P4 only |

`st7701` is the odd one out: it is a DSI panel, so there is no pin list, and it
is the only loader that reports failure — a DSI link can fail for reasons a
pin list cannot (the PHY LDO, the lane rate, the panel not answering).

```lua
vesc.gpio_configure(26, "out")
vesc.gpio_write(26, false)          -- backlight, active-LOW on this board
vesc.disp_load("st7701", 27, 500)
vesc.disp_orientation(1)            -- 480x800 native -> landscape
```

`vesc.disp_orientation(0..3)` works only on panels whose driver can rotate,
and raises otherwise rather than accepting the call and doing nothing — which
is how a dash ends up sideways with no error anywhere.

Loading registers the driver with the display backend, which is what makes the
drawing functions start reaching the glass. An unknown panel name lists the
available ones in the error rather than leaving you to guess at spellings.

Not loadable this way yet: `st7701_rgb`, whose configuration is a struct
rather than a pin list, and any board whose own `hw_*.c` provides a
`disp-init`. Those boards are also not buildable with Lua yet.

### Drawing

Buffers are Lua objects with methods, and the drawing is tinygfx — the same
code the lisp engine draws with, so both engines produce identical pixels.

```lua
local b = vesc.img_buffer("indexed4", 240, 60)
b:clear(0)
b:rectangle(0, 0, 240, 60, 1, true)
b:line(0, 30, 239, 30, 2)
b:circle(120, 30, 20, 3)
vesc.disp_render(b, 0, 0, {0x000000, 0x202020, 0x00FF00, 0xFFFFFF})
```

| method | notes |
|---|---|
| `vesc.img_buffer(fmt, w, h)` | `indexed2/4/16`, `rgb332`, `rgb565`, `rgb888` |
| `b:dims()` | width, height |
| `b:clear([c])` | |
| `b:setpix(x, y, c)` | clipped |
| `b:getpix(x, y)` | `nil` off-buffer |
| `b:line(x0, y0, x1, y1, c, [thick], [dot1], [dot2])` | |
| `b:rectangle(x, y, w, h, c, [filled], [thick], [radius])` | |
| `b:circle(x, y, r, c, [filled], [thick])` | |
| `b:arc(x, y, r, a0, a1, c, [thick], [rounded], [filled], [sector], [segment])` | |
| `b:triangle(x0,y0, x1,y1, x2,y2, c)` | filled |
| `b:blit(src, x, y, [transparent])` | |
| `vesc.disp_render(b, x, y, [palette])` | false if no driver |
| `vesc.disp_clear([c])`, `vesc.disp_reset()`, `vesc.disp_loaded()` | |

Two differences from lisp worth knowing:

**No pool to declare.** LispBM allocates image buffers from a defrag pool the
script creates and sizes by hand (`dm-create`), and getting that size wrong
fails later, at the point of drawing. Here a buffer is ordinary Lua userdata:
counted against the memory ceiling, collected when the last reference goes,
and a buffer too large for the ceiling raises out-of-memory at the point of
allocation instead.

**Indexed formats are worth using.** For an indexed buffer the palette is
supplied at render time, so the same drawing can be rendered in different
colours without redrawing it, and an `indexed4` buffer is a quarter the size
of `rgb565` for the same area. On a dash drawing in a handful of colours that
is the difference between fitting comfortably and not.

Drawing off the edge of a buffer is clipped, not an error — a dash positions
elements by arithmetic and running slightly off is routine. Reading off the
edge returns `nil` rather than a pixel from the next row.

`vesc.disp_render` returns `false` when no panel driver is loaded rather than
raising, so a script on a board whose display failed to initialise can carry
on doing everything else.

### Text

```lua
local font = vesc.font_load(vesc.asset("font24"))
local w, h = font:measure("48.2V")
buf:text(x, baseline_y, font, "48.2V", colour, antialias)
local asc, desc, gap, glyphs = font:metrics()
```

Fonts are the ones `ttf-prepare` produces: glyphs already rasterised at one
size, with metrics and optional kerning, in a single blob. Rendering one needs
no TrueType parsing, which is why text works here while preparing a font does
not — that needs `schrift`, which allocates through LispBM's memory pool.
Prepare fonts on a host or a lisp build and bundle the result.

`y` is the **baseline**, not the top. That is what the metrics are relative
to, and placing text by its top makes lines of different sizes fail to line
up.

`measure` and `text` share one glyph walk, so they cannot disagree about
advances or kerning. A missing glyph is skipped rather than failing the
string: one unexpected character should not blank a field.

Antialiasing falls out of the glyph format. An `indexed4` font carries three
coverage levels, and coverage maps onto **consecutive palette entries** from
the colour given — so a palette with a ramp gets smooth text:

```lua
local PAL = {0x000000, 0x404040, 0x909090, 0xFFFFFF}
buf:text(10, 40, font, "hello", 1, true)    -- uses entries 1..3
buf:text(10, 80, font, "hello", 3, false)   -- hard edges, entry 3 only
```

### Bundling binary data

`require` compiles Lua source, so fonts, icons and lookup tables need another
route. The container's import table holds arbitrary bytes:

```
tools/luapack.py main.lua -o main.luapkg     --asset font24=font/roboto-bold-24-4c.bin     --asset logo=img/logo.bin
```

```lua
local blob = vesc.asset("font24")     -- a string, or nil if not bundled
```

The board has no filesystem, so the container is the only thing that travels
with a script. Assets are returned as counted strings, so a NUL inside one
does not truncate it.

### Touch

```lua
vesc.touch_load_gt911(sda, scl, rst, int_pin, width, height, [hz])
vesc.touch_transform(swap_xy, mirror_x, mirror_y)
local x, y, strength = vesc.touch_read()   -- nil when nothing is touching
vesc.touch_unload()
vesc.touch_loaded()
```

`touch_read` returns `nil` rather than the last coordinates when nothing is
touching, because a dash needs to know a finger has lifted and a stale pair
looks exactly like a finger that has stopped moving.

**Pass the panel's native size, not the rotated size.** On the Waveshare P4
Touch LCD 4.3 the panel is 480x800 driven as 800x480, and the working
configuration is:

```lua
vesc.touch_load_gt911(7, 8, 23, -1, 480, 800)   -- native bounds
vesc.touch_transform(true, false, true)         -- swap_xy, mirror_y
```

`esp_lcd_touch` computes mirroring and clamping in the controller's own frame
*before* any swap is applied, so handing it the rotated size makes the
arithmetic correct only where the error cancels. That presents as touch that
works in the middle of the screen and inverts towards the edges — which reads
as a panel needing calibration rather than as a wrong parameter.

`int_pin` can be `-1`, and on several boards it has to be: the GT911 samples
that pin as its reset is released to choose between I2C address 0x5D and 0x14,
so a board that pulls it up answers at the other address from the one the
driver expects.

Not bound: interrupt-driven touch events, multi-touch, and the four other
controllers the lisp engine supports. Polling one point covers a dash, and a
script polling at its own rate is easier to reason about than one woken by an
interrupt.

### Events

Handlers run on the engine task, one at a time. The frame or payload is
copied before it reaches you, so it stays valid.

```lua
vesc.on_can(function(id, data, is_ext, bus)
  if id == 0x123 then
    local a, b = string.unpack("<i2i2", data)
  end
end)

vesc.on_app_data(function(data) end)

vesc.on_timer(50, function() end)    -- period in ms, one timer
```

Events arriving faster than handlers can run them are **dropped, not
queued indefinitely**: the producers are the CAN and comms tasks, and blocking
those because a script is slow would turn a script problem into a comms
problem. `vesc.events_dropped()` counts them, because only your handler knows
whether a gap matters — sampling a sensor can tolerate one, counting wheel
pulses cannot.

A handler that raises is reported and stays registered. One malformed frame
will not silently unsubscribe you from the bus.

## Limits

- **Memory** is capped at 192 KB. Past that, allocation fails and raises a
  normal Lua error, so a runaway script fails rather than starving the
  firmware. Reported through `COMM_LISP_GET_STATS`, which VESC Tool shows.
- **A script is interruptible.** The VM is hooked every 2000 instructions, so
  `while true do end` can still be stopped from VESC Tool.
- **Numbers are 32-bit**: `lua_Number` is a float and `lua_Integer` an int32,
  matching the hardware. Integer arithmetic wraps at 2^31.
- **No `require` of anything not bundled.** There is no filesystem.

## What is not bound yet

Touch, BLE, wifi, rgbled, BMS and IMU., BLE, wifi, rgbled, BMS and IMU. Those lisp extensions are
written against the LispBM value ABI, and porting them is the bulk of the
remaining work; the drivers underneath are engine-neutral. Boards whose own
`hw_*.c` registers script extensions cannot be built with Lua yet for the
same reason. `tools/script_ext_coverage.py` is the current state of that.

## A worked example

`examples/lua/dash.lua` is a working dash page: a top bar, a large speed
readout and three stacked readings, updating at 10 Hz. Measured on an
ESP32-P4 at 800x480: **2.36% CPU and 85 kB** of the engine's 192 kB ceiling.

Three things in it are worth copying rather than rediscovering:

- **A buffer per field, not one for the screen.** A full-screen render here is
  384 000 pixels through a software transpose. Each field owns a buffer sized
  to itself, so a changed reading costs a render of its own box.
- **Change detection on the formatted string, not the value.** A current
  reading that wobbles in the third decimal must not force a redraw every
  frame. Idle, the dash redraws nothing.
- **Age-gate every CAN reading.** The first run of that dash on hardware
  showed a confident `0.0 V`, because `canget_vin` returned the last value
  seen for an id that had only ever reported zeros. Everything goes through
  `vesc.can_msg_age` now and anything stale reads as absent.

The layout is a table of `{x, y, read, fmt, colour}` rows, so adding a reading
is data rather than code.

## Testing without hardware

```
make -C main/script/test run
```

Runs the container parser, the engine and the packer, including an end-to-end
case where the Python packer's output is parsed by the C parser and executed
by the interpreter. Under ASan and UBSan. `make -C main/script/test fuzz`
fuzzes the container parser, which matters because the container arrives over
USB or BLE and every offset in it is attacker-controlled.
