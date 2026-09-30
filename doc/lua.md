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

Display, touch, BLE, wifi, rgbled, BMS and IMU. Those lisp extensions are
written against the LispBM value ABI, and porting them is the bulk of the
remaining work; the drivers underneath are engine-neutral. Boards whose own
`hw_*.c` registers script extensions cannot be built with Lua yet for the
same reason. `tools/script_ext_coverage.py` is the current state of that.

## Testing without hardware

```
make -C main/script/test run
```

Runs the container parser, the engine and the packer, including an end-to-end
case where the Python packer's output is parsed by the C parser and executed
by the interpreter. Under ASan and UBSan. `make -C main/script/test fuzz`
fuzzes the container parser, which matters because the container arrives over
USB or BLE and every offset in it is attacker-controlled.
