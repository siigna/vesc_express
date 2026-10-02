# ESCargot Express

Firmware for WiFi- and Bluetooth-enabled logger, IO and display boards built on
the ESP32. A fork of the VESC® Express firmware, with a Lua script engine and
a display stack added. Tested on the ESP32-C3, C6, S3 and P4.

**Not affiliated with, endorsed by, or certified by Mr. Benjamin Vedder.**
VESC® is his registered trademark; see [TRADEMARKS.md](TRADEMARKS.md). This
firmware is compatible with VESC® Tool, which is what uploads scripts and
writes the configuration.

## What this fork adds

### A Lua script engine

Runs alongside LispBM rather than replacing it here — the ESP32 has the flash
for both. Lua 5.4 in 32-bit mode, with the same container format and the same
`COMM_LISP_*` packets, so VESC® Tool's existing upload, erase and REPL carry
Lua unchanged.

The engine itself has no firmware dependencies: everything it needs from its
host arrives through a config struct as a function pointer, which is what lets
`main/script/test` exercise the sandbox, the memory ceiling, the interrupt hook
and `require` resolution on a host. A script engine that can only be tested by
flashing is one whose failure modes get found on a vehicle.

`main/script/luaif.c` is the firmware adapter. The engine core
(`script_lua.c`, `script_pack.c`, `script_event.h`) is kept **byte-identical**
with the controller firmware's copy, so one interpreter runs on both a display
and a motor controller.

### A display stack

Drivers for ST7701 RGB, JD9165, ICNA3306 and others; a TTF renderer; Clay
layout; an offline render harness that produces PNGs on a host with no
hardware, which is how the dash pages are regression-tested pixel by pixel.

### Bring-up and diagnostics

A 64-line log ring readable from a script or the terminal, touch statistics,
and `tools/vesc_script.py` for driving a board over USB — upload, REPL, app
data, log tail.

## Tests

```bash
cd main/script/test && make && ./test_script_lua packed.bin
cd main/display/test && make && ./test_rgb_convert
```

All host-side, no board required: 142 checks on the engine, 42 on the
container, 72 on the pixel conversions.

## Upstream

`upstream` points at `vedderb/vesc_express` over HTTPS and is pull-only; its
push URL is deliberately set to `no-push`. Changes go to `origin`.

## Toolchain

Instructions for how to set up the toolchain can be found here:
[https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/linux-macos-setup.html](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/linux-macos-setup.html)

### Get Release 5.5.4

The instructions linked above will install the master branch of ESP-IDF. To install the stable release you can navigate to the installation directory and use the following commands:

```bash
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.4
cd esp-idf-v5.5.4/
./install.sh esp32c3 esp32c6 esp32s3
```

At the moment development is done using the stable 5.5.4-release. Note that different IDF-versions are very likely to cause compatibility issues, so it is strongly recommended to use version 5.5.4.

## Building

Set the target chip/architecture with 
```bash
idf.py set-target <target> 
```

where target is esp32c3, esp32c6 or esp32s3. You will need to run a fullclean or remove the build directory when changing targets.

Each normal build target uses its own shared 4 MB base file `sdkconfig.defaults.<target>`.

Boards that need non-default flash or PSRAM settings should instead provide their own full `sdkconfig.defaults.<hw_file>` file next to the shared target configs in the repository root.

Once the toolchain is set up in the current path, the project can be built with

```bash
idf.py build
```

That will create vesc_express.bin in the build directory, which can be used with the bootloader in VESC® Tool. If the ESP32c3 does not come with firmware preinstalled, the USB-port can be used for flashing firmware using the built-in bootloader. That also requires bootloader.bin and partition-table.bin which also can be found in the build directory. This can be done from VESC® Tool or using idf.py.

All targets can be built with

```bash
python build_all.py
```

That will create all required firmware files under the build_output directory, with hardware names as child directories. All target switching is handled automatically with the build_all command.

### Custom Hardware Targets

If you wish to build the project with custom hardware config files you should add the hardware config files to the "**main/hwconf**" directory and use the HW_NAME build flag
```bash
idf.py build -DHW_NAME="VESC Express T"
```

**Note:** If you ever change the environment variables, or if when you first start using them, you need to first run `idf.py reconfigure` before building (with the environment variables still set of course!), as the build system unfortunately can't automatically detect this change. Running `idf.py fullclean` has the same effect as this forces cmake to rebuild the build configurations.
