set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES )

set(COMPONENT_SRCS
"main.c"
"buffer.c"
"comm_uart.c"
"comm_usb.c"
"comm_can.c"
"comm_ble.c"
"comm_wifi.c"
"packet.c"
"crc.c"
"commands.c"
"conf_custom.c"
"terminal.c"
"mempools.c"
"log.c"
"adc.c"
"ublox.c"
"nmea.c"
"utils.c"
"flash_helper.c"
"rb.c"
"bms.c"
"log_comm.c"
"digital_filter.c"

"config/confsrc.c"
"hwconf/hw.c"

"lispif.c"
"lispif_c_lib.c"
"lispif_events.c"
"lbm_vesc_utils.c"
"lispif_vesc_extensions.c"
"lbm_color_extensions.c"
"lispBM/src/env.c"
"lispBM/src/fundamental.c"
"lispBM/src/heap.c"
"lispBM/src/lbm_memory.c"
"lispBM/src/print.c"
"lispBM/src/stack.c"
"lispBM/src/symrepr.c"
"lispBM/src/tokpar.c"
"lispBM/src/extensions.c"
"lispBM/src/lispbm.c"
"lispBM/src/eval_cps.c"
"lispBM/platform/freertos/src/platform_mutex.c"
"lispBM/platform/freertos/src/platform_timestamp.c"
"lispBM/src/lbm_channel.c"
"lispBM/src/lbm_c_interop.c"
"lispBM/src/lbm_custom_type.c"
"lispBM/src/lbm_flat_value.c"
"lispBM/src/lbm_prof.c"
"lispBM/src/lbm_defrag_mem.c"
"lispBM/src/lbm_image.c"
"lispBM/src/extensions/array_extensions.c"
"lispBM/src/extensions/math_extensions.c"
"lispBM/src/extensions/string_extensions.c"
"lispBM/src/extensions/display_extensions.c"
"lispBM/src/extensions/mutex_extensions.c"
"lispBM/src/extensions/lbm_dyn_lib.c"
"lispBM/src/extensions/ttf_extensions.c"
"lispBM/src/extensions/schrift.c"
"lispBM/src/extensions/ttf_backend.c"
"lispBM/utils/tjpgd.c"
"lispBM/utils/tinygfx.c"

"wifi/lispif_wifi_extensions.c"

"ble/custom_ble.c"

"rgbled/lispif_rgbled_extensions.c"

"touch/lispif_touch_extensions.c"
"touch/touch_cst836u.c"

"display/lispif_disp_extensions.c"
"display/disp_sh8501b.c"
"display/disp_ili9341.c"
"display/disp_ssd1306.c"
"display/disp_st7789.c"
"display/disp_st7789a.c"
"display/disp_ili9488.c"
"display/disp_st7735.c"
"display/disp_ssd1351.c"
"display/disp_icna3306.c"
"display/disp_sh8601.c"
"display/disp_axs15231.c"
"display/disp_gc9a01.c"
"display/disp_jd9853.c"
"display/disp_jd9165.c"
"display/disp_st7701.c"
"display/disp_st7701_rgb.c"
"display/clay_layout.c"

"drivers/bme280/bme280.c"
"drivers/bme280/bme280_if.c"
"drivers/hwspi.c"
"drivers/spi_bb.c"
"drivers/enc_as504x.c"
"drivers/imu/imu.c"
"drivers/imu/lsm6ds3.c"
"drivers/imu/ahrs.c"

"lowzip/lowzip.c"
)

# --------------------------------------------------------------------------
# Script engine selection.
#
#   idf.py -DSCRIPT_ENGINE=lua -DHW_NAME="S3 N16R8" build
#
# Default is lisp, which is the configuration everything else in this tree
# expects, and selecting it changes nothing. Choosing lua swaps the whole
# scripting subsystem: LispBM, its VESC extension bindings and the adapter in
# lispif.c come out; Lua, the shared container parser and the adapter in
# main/script/luaif.c go in.
#
# The two adapters implement the same eight entry points, so commands.c,
# comm_can.c and main.c are untouched either way and do not know which
# language the board runs.
#
# A lua build also drops the display, touch, BLE, wifi and rgbled extension
# modules, and the board files that register their own lisp extensions. Those
# are written against the LispBM value ABI, which is the real work in porting
# them -- see tools/script_ext_coverage.py for what is bound so far. Boards
# with no lisp glue of their own build as they are.
set(SCRIPT_ENGINE "lisp" CACHE STRING "Script engine: lisp or lua")
set_property(CACHE SCRIPT_ENGINE PROPERTY STRINGS lisp lua)

if(SCRIPT_ENGINE STREQUAL "lua")
    # Announced only in the real configure pass. IDF also runs this file in
    # cmake script mode to scan requirements, where the -D is not passed, so
    # printing unconditionally reports the wrong engine once per build.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        message(STATUS "Script engine: Lua (LispBM excluded)")
    endif()

    # Everything written against the LispBM ABI.
    set(LBM_DEPENDENT
        "lispif.c"
        "lispif_c_lib.c"
        "lispif_events.c"
        "lbm_vesc_utils.c"
        "lispif_vesc_extensions.c"
        "lbm_color_extensions.c"
        "wifi/lispif_wifi_extensions.c"
        "rgbled/lispif_rgbled_extensions.c"
        "touch/lispif_touch_extensions.c"
        "touch/touch_cst836u.c"
        "display/lispif_disp_extensions.c"
        "display/clay_layout.c"
    )
    foreach(src ${COMPONENT_SRCS})
        # The lispBM tree itself, and the display drivers, which register
        # their own extensions alongside the driver code.
        if(src MATCHES "^lispBM/" OR src MATCHES "^display/disp_")
            list(APPEND LBM_DEPENDENT ${src})
        endif()
    endforeach()

    foreach(src ${LBM_DEPENDENT})
        list(REMOVE_ITEM COMPONENT_SRCS ${src})
    endforeach()

    file(GLOB LUA_CORE_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "lua/*.c")
    list(APPEND COMPONENT_SRCS
        ${LUA_CORE_SRCS}
        "script/script_pack.c"
        "script/script_lua.c"
        "script/lua_vesc_ext.c"
        "script/luaif.c"
    )

else()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        message(STATUS "Script engine: LispBM")
    endif()
endif()

if(CONFIG_BT_BLUEDROID_ENABLED)
    list(APPEND COMPONENT_SRCS
        "ble/lispif_ble_extensions.c"
    )
endif()

set(COMPONENT_ADD_INCLUDEDIRS
"."
"config"
"lua"
"script"
"hwconf"
"hwconf/trampa"
"hwconf/trampa/bms_rb"
"hwconf/vesc"
"hwconf/vesc/vbms32"
"hwconf/vesc/vdisp_dual"
"hwconf/vesc/scope"
"hwconf/vesc/duet"
"hwconf/vesc/link"
"hwconf/vesc/vdisp_900"
"hwconf/vesc/nanolog"
"hwconf/vesc/vbms16"
"hwconf/vesc/rmcore"
"hwconf/vesc/dash16"
"hwconf/vesc/wcore"
"hwconf/rescue"
"hwconf/avaspark"
"hwconf/vesc/vbms_inf_sat"
"hwconf/seeed"
"hwconf/waveshare/s3_touch_lcd4"
"hwconf/waveshare/p4_touch_lcd43"

"lispBM"
"lispBM/include"
"lispBM/platform/freertos/include"
"lispBM/utils"
"display"
"display/clay"
"wifi"
"ble"
"rgbled"
"touch"
"drivers"
"drivers/bme280"
"drivers/imu"
"lowzip"
)

register_component()

# After registration: during IDF's requirements scan this file is run in cmake
# script mode, where add_compile_definitions does not exist, and the scan stops
# at register_component(). Anything using it therefore has to sit below here --
# which is why the LBM_USE_* block further down works.
if(SCRIPT_ENGINE STREQUAL "lua")
    add_compile_definitions(SCRIPT_ENGINE_LUA=1)
else()
    add_compile_definitions(SCRIPT_ENGINE_LISP=1)
endif()

# Clay is ~80 kB of flash and is only usable on a board with a display, so it
# is enabled for the targets with room rather than for everything. The C3 in
# particular has very little app partition left.
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    add_compile_definitions(HW_USE_CLAY)
endif()

add_compile_definitions(
    LBM_USE_DYN_FUNS
    LBM_USE_DYN_MACROS
    LBM_USE_DYN_LOOPS
    LBM_USE_DYN_ARRAYS
    LBM_USE_TIME_QUOTA
    LBM_USE_ERROR_LINENO
    LBM_USE_MACRO_REST_ARGS
)

if((DEFINED ENV{HW_SRC}) OR (DEFINED ENV{HW_HEADER}))
    if(NOT DEFINED ENV{HW_SRC})
        message(FATAL_ERROR "HW_SRC not defined while HW_HEADER is set. You must either set both or none.")
    endif()
    if(NOT DEFINED ENV{HW_HEADER})
        message(FATAL_ERROR "HW_HEADER not defined while HW_SRC is set. You must either set both or none.")
    endif()
    
    message(STATUS "Setting HW_SOURCE=\"$ENV{HW_SRC}\" and HW_HEADER=\"$ENV{HW_HEADER}\" definitions")
    
    add_compile_definitions(
        HW_SOURCE=\"$ENV{HW_SRC}\"
        HW_HEADER=\"$ENV{HW_HEADER}\"
    )
endif()
