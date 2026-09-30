# Vendored Lua

Lua 5.4.7, from https://www.lua.org/ftp/lua-5.4.7.tar.gz, MIT licensed. The
copyright notice is at the bottom of `lua.h` and applies to every file here.

## What was left out

Not copied, so they cannot be reached from a script even by accident:

| omitted | why |
|---|---|
| `liolib.c` | filesystem access |
| `loslib.c` | `os.execute`, `os.exit`, the clock and the environment |
| `loadlib.c` | loads native libraries at runtime |
| `ldblib.c` | the debug library reaches around every other restriction |
| `lutf8lib.c` | unused, and it is flash |
| `linit.c` | replaced by the sandbox in `../script/script_lua.c`, which opens a chosen subset |
| `lua.c`, `luac.c` | the standalone interpreter and compiler, which are host tools |

`main/script/script_lua.c` additionally removes `dofile`, `loadfile`, `load`
and `collectgarbage` from the base library after opening it. The first two want
a filesystem; `load` would let a script compile a string at runtime, which
defeats any review of what was actually flashed; `collectgarbage` would let a
script switch off the collector on a device where the memory ceiling is the
only thing keeping it away from the comms stack.

## The one local change

`luaconf.h`, `LUA_32BITS` set to 1 (upstream ships 0). That makes `lua_Number`
a float and `lua_Integer` an int32, which matches both targets: they are
32-bit with single-precision FPUs, and leaving it at 0 pulls in
double-precision softfloat for no benefit.

The change is marked with a `VESC:` comment in the file. Keeping it there
rather than defining `LUA_32BITS` on the command line is deliberate — Lua
defines the macro itself, so a command-line define collides with it.

## Measured footprint

| target | flags | flash |
|---|---|---|
| Cortex-M4 (`bldc`) | `-Os -mcpu=cortex-m4 -mfloat-abi=hard` | 87,456 B |
| Xtensa ESP32-S3 (`vesc_express`) | `-Os` | 90,638 B |

For comparison, the LispBM core it replaces measures 86,974 B of flash on the
same Cortex-M4 build. Static RAM for Lua is zero: it allocates everything
through the ceiling-enforcing allocator in `../script/script_lua.c`.

## Updating

Replace the `.c` and `.h` files from a release tarball, omitting the files in
the table above, then re-apply the `LUA_32BITS` change. `main/script/test`
will tell you quickly if anything moved.
