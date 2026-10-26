# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ====================================================================================
# CORRECTED: the actual hardware is a plain Pico 2 (RP2350), NOT a
# Pico 2 W -- confirmed directly by the user. This was building against
# the WRONG board definition the entire time. Board-specific header
# files in the Pico SDK set board-specific defaults (clock/crystal
# configuration, default pin assignments, etc.) that a "_w" variant's
# own headers may specify differently than what's actually present on
# a plain Pico 2 -- SPI baud rates are computed from the system clock
# frequency, so a mismatched clock config here could produce SPI timing
# subtly different from what the code intends, without any of the
# actual bytes sent being wrong -- exactly the kind of thing that
# wouldn't show up in a byte-level SPI transaction log. This was never
# caught earlier because none of the isolated stress-test rounds (touch,
# HP-IL, bitmap draws, LED timer) tested the board definition itself.
set(PICO_BOARD pico2 CACHE STRING "Board type")

# SPDX-License-Identifier: MIT
#
# CMakeLists.txt for the hipi project, structured to build under
# the "Raspberry Pi Pico" VS Code extension.
#
# The VS Code extension invokes cmake with a toolchain file that already
# sets PICO_SDK_PATH, PICO_BOARD, PICO_PLATFORM, and the ARM cross-compiler.
# We just need to call pico_sdk_init() and add our targets.

cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------
# Locate & initialise the Pico SDK.
#
# The VS Code extension pre-sets PICO_SDK_PATH via its toolchain file.
# If you're building from the command line without the extension, either:
#   - export PICO_SDK_PATH=/path/to/pico-sdk, or
#   - pass -DPICO_SDK_PATH=/path/to/pico-sdk on the cmake command line.
# ----------------------------------------------------------------------
if(DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if(NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set.  Set it as an env var or pass "
        "-DPICO_SDK_PATH=/path/to/pico-sdk to cmake.")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" ABSOLUTE)

# This file lives inside the pico-sdk checkout and brings in all the
# helper functions (pico_enable_stdio_usb, pico_add_extra_outputs, etc.).
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")

project(hipi_pico C CXX)

# ----------------------------------------------------------------------
# Display panel: HIPI has one, the 7" 1024x600 panel with an LT7683
# display controller and FT5316 capacitive touch. The firmware target is
# hipi_7_pico (hipi_7_pico.uf2/.elf/...); the name is kept so release
# files stay named as before.
# ----------------------------------------------------------------------

# Initialise the Raspberry Pi Pico SDK
# C++ exceptions: PicoLED (the NeoPixel library) throws them. The SDK reads
# this while initialising, so it must be set BEFORE pico_sdk_init() --
# set afterwards it has no effect ("exception handling disabled").
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
pico_sdk_init()

# HIPI needs an RP2350 board -- Raspberry Pi Pico 2 (PICO_BOARD=pico2) or
# Pico 2 W (PICO_BOARD=pico2_w; its wireless chip isn't used), ARM cores.
# A firmware built for another board/platform copies onto the BOOTSEL drive
# fine but is then ignored by the RP2350 -- the board doesn't restart.
message(STATUS "HIPI: board ${PICO_BOARD}, platform ${PICO_PLATFORM}")
if(NOT PICO_PLATFORM STREQUAL "rp2350-arm-s")
    message(FATAL_ERROR
        "HIPI must be built for the Raspberry Pi Pico 2 / Pico 2 W (RP2350, ARM) -- "
        "this build is for board '${PICO_BOARD}', platform '${PICO_PLATFORM}'. "
        "Choose board pico2 (or pico2_w) -- in VS Code: Raspberry Pi Pico: Switch Board; "
        "on the command line: -DPICO_BOARD=pico2 -- and configure again from an empty build folder.")
endif()

# 1. Include the helper file
include(${PICO_SDK_PATH}/tools/Findpioasm.cmake)

# 2. It locates pioasm and sets the PIOASM variable
# (Findpioasm.cmake does this automatically if pioasm is in PATH)
# Otherwise, set the path explicitly:
set(PIOASM ${PICO_SDK_PATH}/tools/pioasm/pioasm)    # adjust the path if needed

# ----------------------------------------------------------------------
# Project-wide compile flags.
# ----------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)

## # Declare CFG_TUD_CDC globally for the whole build
## # Force CFG_TUD_CDC=2 for the whole build, incl. the SDK's tinyusb_device.a
## add_compile_definitions(CFG_TUD_CDC=2)
## add_compile_definitions(CFG_TUD_CDC_RX_BUFSIZE=256)
## add_compile_definitions(CFG_TUD_CDC_TX_BUFSIZE=256)

# ----------------------------------------------------------------------
# Shared support library: the SD-card/FatFS code.
# ----------------------------------------------------------------------
add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src build)

include("PicoLED/PicoLed.cmake")

# ----------------------------------------------------------------------
# The version, from git: cmake/hipi_version.cmake writes
# hipi_generated/hipi_version.h (HIPI_VERSION_TEXT / _FULL) -- now, so it's
# there for the first build, and again at every build, so a new commit or
# tag shows up without re-running CMake. The header is only rewritten when
# the version changes, and only src/version.cpp includes it.
# ----------------------------------------------------------------------
set(HIPI_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/hipi_generated/hipi_version.h)
set(HIPI_VERSION_ARGS -DOUT=${HIPI_VERSION_HEADER} -DPREFIX=HIPI -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hipi_version.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${HIPI_VERSION_ARGS})
add_custom_target(hipi_version ALL
    COMMAND ${CMAKE_COMMAND} ${HIPI_VERSION_ARGS}
    BYPRODUCTS ${HIPI_VERSION_HEADER}
    COMMENT "HIPI version from git"
    VERBATIM)

# ----------------------------------------------------------------------
# The target chain: core lib -> Pico transport lib -> Pico executable.
# ----------------------------------------------------------------------
set(CORE_TARGET      hipi_core)
set(TRANSPORT_TARGET hipi_pico_transport)
set(EXE_TARGET       hipi_7_pico)

# ---- Core library -- LT7683 driver + HP82163 Screen class
# (platform-neutral). ----
add_library(${CORE_TARGET} STATIC
    src/LT7683.cpp
    src/Screen.cpp
)
target_include_directories(${CORE_TARGET} PUBLIC include)

# ---- Pico-specific transport — links against pico-sdk's SPI/GPIO hw
# libs. ----
add_library(${TRANSPORT_TARGET} STATIC
    src/PicoSpiTransport.cpp
)
target_link_libraries(${TRANSPORT_TARGET} PUBLIC
    ${CORE_TARGET}
    pico_stdlib
    hardware_spi
    hardware_gpio
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)
target_include_directories(${TRANSPORT_TARGET} PUBLIC include)

# ---- The built-in I2C chip list: sdcard/i2c/chips.txt as a string ----
# (i2c_ident.cpp uses it when the SD card has no i2c/ folder). Changing
# chips.txt re-runs CMake by itself.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/sdcard/i2c/chips.txt HIPI_I2C_CHIPS_TEXT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sdcard/i2c/chips.txt)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/i2c_chips_default.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/hipi_generated/i2c_chips_default.cpp @ONLY)

# ---- Firmware executable — flashed to the board. ----
add_executable(${EXE_TARGET}
    src/pico_main.cpp
    src/boardui.cpp
    src/hipi.cpp
    src/hpil.cpp
    src/device.cpp
    src/drive.cpp
    src/display.cpp
    src/illeds.cpp
    src/ilpixels.cpp
    src/touch.cpp
    src/leds.cpp
    src/pixels.cpp
    src/i2c_device.cpp
    src/bmp280.cpp
    src/pilbox.cpp
    src/plotter.cpp
    src/plotterview.cpp
    src/printer.cpp
    src/printer_font.cpp
    src/video80.cpp
    src/hwtest.cpp
    src/hwtest_io.cpp
    src/i2c_ident.cpp
    src/i2c_chips.cpp
    src/i2c_command.cpp
    src/ili2c.cpp
    src/vl53l0x.cpp             # VL53L0X set-up and measurement (TFI2C "MD")
    src/lif_create.cpp          # File picker -> Create media: an empty LIF image
    src/media_create_ui.cpp     # ... its dialogs (CREATE MEDIA, MEDIA NAME)
    src/mediamap.cpp            # Tape view -> Media map: the medium as rings
    src/printout.cpp            # Printer: the torn-off paper saved (BMP + text)
    ${CMAKE_CURRENT_BINARY_DIR}/hipi_generated/i2c_chips_default.cpp
    src/demo.cpp
    src/demo_content.cpp
    src/terminal.cpp
    src/hw_config.cpp
    src/my_descriptors.c
    src/usb_msc.cpp
    src/screendump.cpp
    src/display_text.cpp        # Display: its output kept as plain text
    src/display_text_save.cpp   # ... in the SDRAM, saved to the SD card
    src/mirror_upload.cpp       # Files from hipiview (a LIF volume dropped on the Tape view)
    src/mirror_download.cpp     # A cassette to hipiview (Tape view -> Save media on PC)
    src/buttons_image.cpp       # built-in button strip, see scripts/bmp_to_rgb565.py
    src/bootscreen.cpp
    src/analyzer.cpp
    src/loopmap.cpp
    src/chartable.cpp
    src/chartable_font.cpp      # generated, see scripts/ttf_to_glyphs.py
    src/hpil_diag.cpp
    src/hpil_signals.cpp        # the signals view drawing (and its example)
    src/devicelist.cpp          # the HP-IL DEVICES list (corner touch)
    src/backlight.cpp
    src/clock.cpp
    src/rtc.cpp
    src/rtc_probe.cpp           # the clock chips' checks (Hardware test, TFI2C "I")
    src/glyph_font.cpp
    src/clock_font_big.cpp      # generated, see scripts/ttf_to_glyphs.py
    src/clock_font_small.cpp    # generated, see scripts/ttf_to_glyphs.py
    src/menu_font_title.cpp     # generated, see scripts/ttf_to_glyphs.py
    src/saver_anim.cpp
    src/display_mirror.cpp      # display mirrored to a PC, see tools/hipiview
    src/saver_font_rain.cpp     # generated, see scripts/ttf_to_glyphs.py
    src/saver_font_goose40.cpp  # generated (5 goose sizes), see scripts/ttf_to_glyphs.py
    src/saver_font_goose56.cpp
    src/saver_font_goose72.cpp
    src/saver_font_goose92.cpp
    src/saver_font_goose112.cpp
    src/logo_image.cpp          # built-in logo, see scripts/bmp_to_rgb565.py
    src/version.cpp             # the version from git, see cmake/hipi_version.cmake
    src/crashlog.cpp            # a HardFault: kept over a restart, logged (crashlog.h)
)
target_include_directories(${EXE_TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/hipi_generated)
add_dependencies(${EXE_TARGET} hipi_version)

target_link_libraries(${EXE_TARGET} PRIVATE
    ${CORE_TARGET}
    hardware_i2c
    hardware_spi
    hardware_pwm
    hardware_gpio
    ${TRANSPORT_TARGET}
    pico_stdlib
    pico_rand               # screen saver animations (saver_anim.cpp)
    hardware_dma            # HP-IL signal scope (hpil_diag.cpp)
    hardware_exception      # the HardFault handler (crashlog.cpp)
    hardware_watchdog       # ... and the restart after it
    tinyusb_device          # ← replace pico_stdio_usb with this
    tinyusb_board
#    pico_bootsel_via_double_reset
    # pico_cyw43_arch_none   # Only needed if targeting a "_w" board
                              # variant with the CYW43 wifi/bt chip --
                              # not applicable now that PICO_BOARD is
                              # correctly set to plain "pico2" above.
    PicoLed
)

target_compile_definitions(${EXE_TARGET} PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR=0
    PICO_PANIC_FUNCTION=hipi_panic   # a panic is kept and HIPI restarts (crashlog.cpp)
)

# Generate header from the .pio file
pico_generate_pio_header(${EXE_TARGET}
    ${CMAKE_CURRENT_LIST_DIR}/src/hpil.pio
)
pico_generate_pio_header(${EXE_TARGET}
    ${CMAKE_CURRENT_LIST_DIR}/src/hpil_diag.pio
)

pico_enable_stdio_usb(${EXE_TARGET} 0)
pico_enable_stdio_uart(${EXE_TARGET} 0)

# Names every generated output (.uf2/.elf/.bin/.hex/.map) after
# EXE_TARGET automatically -- hipi_7_pico.uf2 and so on.
pico_add_extra_outputs(${EXE_TARGET})

# ----------------------------------------------------------------------
# Optional: Linux spidev demo (off by default for Pico-focused work).
# ----------------------------------------------------------------------
option(HIPI_BUILD_LINUX_EXAMPLE "Build the Linux spidev demo" OFF)
if(HIPI_BUILD_LINUX_EXAMPLE)
    add_executable(hipi_linux_demo
        src/LinuxSpiDevTransport.cpp
        src/linux_main.cpp
    )
    target_link_libraries(hipi_linux_demo PRIVATE ${CORE_TARGET})
endif()

# ----------------------------------------------------------------------
# The SD card: HIPI-SD-card.zip in the project folder, with everything a
# new SD card needs -- unzip it onto the card and HIPI is ready. Made by
# sdcard/make_sd_card.py (which can also copy straight onto a card):
#
#   README.txt   from sdcard/
#   resources/   the Tape view pictures, from resources/
#   lif/         the example cassette(s), from sdcard/lif/
#
# Remade by the build whenever one of these files changes. The zip is kept
# in the project folder, not in build/, so it's there for anyone who
# downloads the project without building it. It is reproducible (fixed
# time stamps), so it only changes when its contents do.
# ----------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/sdcard/make_sd_card.py)
    message(STATUS "hipi: sdcard/make_sd_card.py is missing -- HIPI-SD-card.zip is not remade")
elseif(Python3_Interpreter_FOUND)
    file(GLOB HIPI_SD_CASSETTES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/sdcard/lif/*)
    file(GLOB HIPI_SD_I2C CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/sdcard/i2c/*)
    set(HIPI_SD_ZIP ${CMAKE_SOURCE_DIR}/HIPI-SD-card.zip)
    add_custom_command(
        OUTPUT ${HIPI_SD_ZIP}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/sdcard/make_sd_card.py --zip ${HIPI_SD_ZIP}
        DEPENDS ${CMAKE_SOURCE_DIR}/sdcard/make_sd_card.py
                ${CMAKE_SOURCE_DIR}/sdcard/README.txt
                ${HIPI_SD_CASSETTES}
                ${HIPI_SD_I2C}
                ${CMAKE_SOURCE_DIR}/resources/hp82161a.bmp
                ${CMAKE_SOURCE_DIR}/resources/tape-in.bmp
                ${CMAKE_SOURCE_DIR}/resources/open.bmp
                ${CMAKE_SOURCE_DIR}/resources/leds.bmp
                ${CMAKE_SOURCE_DIR}/resources/reels.bmp
                ${CMAKE_SOURCE_DIR}/resources/hp82162a.bmp
                ${CMAKE_SOURCE_DIR}/resources/prtparts.bmp
        COMMENT "Packing the SD card: HIPI-SD-card.zip"
        VERBATIM)
    add_custom_target(hipi_sd_card ALL DEPENDS ${HIPI_SD_ZIP})
else()
    message(STATUS "hipi: no Python 3 -- HIPI-SD-card.zip is not remade")
endif()
