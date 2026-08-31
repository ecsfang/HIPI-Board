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
set(PICO_BOARD pico2_w CACHE STRING "Board type")

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
# Display panel selection.
#
# The Pico firmware is always built for BOTH panels at once (see
# hipi_add_pico_targets() below) -- each gets its own fully separate set
# of libraries and its own executable, named after the panel
# (hipi_5_pico / hipi_7_pico, and their .uf2/.elf/... outputs), so you can
# build both in one `cmake --build build` and flash whichever board you
# actually have on your desk.
#
# HIPI_DISPLAY_PANEL only controls the *optional* Linux spidev demo
# (HIPI_BUILD_LINUX_EXAMPLE, off by default) below, since that one runs
# against whatever single board happens to be wired up locally, e.g.:
#
#   cmake -DHIPI_BUILD_LINUX_EXAMPLE=ON -DHIPI_DISPLAY_PANEL=7INCH -B build
#
#   5INCH -- RA8875 display controller + GSL1680 capacitive touch
#   7INCH -- LT7683 display controller + FT5316 capacitive touch
# ----------------------------------------------------------------------
set(HIPI_DISPLAY_PANEL "5INCH" CACHE STRING "Panel the optional Linux demo targets: 5INCH or 7INCH")
set_property(CACHE HIPI_DISPLAY_PANEL PROPERTY STRINGS 5INCH 7INCH)
if(NOT HIPI_DISPLAY_PANEL STREQUAL "5INCH" AND NOT HIPI_DISPLAY_PANEL STREQUAL "7INCH")
    message(FATAL_ERROR
        "HIPI_DISPLAY_PANEL must be 5INCH or 7INCH, got: ${HIPI_DISPLAY_PANEL}")
endif()

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# 1. Inkludera helper-filen
include(${PICO_SDK_PATH}/tools/Findpioasm.cmake)

# 2. Den letar upp pioasm — sätt variabeln PIOASM
# (Findpioasm.cmake gör detta automatiskt om pioasm finns i PATH)
# Annars sökväg explicit:
set(PIOASM ${PICO_SDK_PATH}/tools/pioasm/pioasm)    # justera sökvägenISCO

# ----------------------------------------------------------------------
# Project-wide compile flags.
# ----------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)

## # Deklarera CFG_TUD_CDC globalt för hela bygget
## # Tvinga CFG_TUD_CDC=2 för hela bygget, inkl. SDK:ns tinyusb_device.a
## add_compile_definitions(CFG_TUD_CDC=2)
## add_compile_definitions(CFG_TUD_CDC_RX_BUFSIZE=256)
## add_compile_definitions(CFG_TUD_CDC_TX_BUFSIZE=256)

# ----------------------------------------------------------------------
# Shared support library, built once regardless of panel choice -- the
# SD-card/FatFS code doesn't depend on which display is attached, and
# add_subdirectory() can't be called twice for the same source dir.
# ----------------------------------------------------------------------
add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src build)

# ----------------------------------------------------------------------
# Defines one full target chain (core lib -> Pico transport lib -> Pico
# executable) for a given panel. Called once per panel below, so both
# hipi_5_pico and hipi_7_pico -- and their respective .uf2/.elf/... build
# outputs, named automatically after the executable target -- exist
# side by side after a single `cmake --build build`.
#
#   PANEL  -- "5INCH" or "7INCH" (becomes -DDISPLAY_<PANEL>, see
#             include/display_config.h)
#   SUFFIX -- short tag used in target/output names, e.g. "5" or "7"
# ----------------------------------------------------------------------
function(hipi_add_pico_targets PANEL SUFFIX)
    set(CORE_TARGET      hipi_core_${SUFFIX})
    set(TRANSPORT_TARGET hipi_pico_transport_${SUFFIX})
    set(EXE_TARGET       hipi_${SUFFIX}_pico)

    # ---- Core library — RA8875/LT7683 drivers + HP82163 Screen class
    # (platform-neutral). Both display drivers are always compiled in;
    # DISPLAY_${PANEL} only decides which one include/display_config.h's
    # DisplayDriver alias actually resolves to for this target chain. ----
    add_library(${CORE_TARGET} STATIC
        src/RA8875.cpp
        src/LT7683.cpp
        src/Screen.cpp
    )
    target_include_directories(${CORE_TARGET} PUBLIC include)
    target_compile_definitions(${CORE_TARGET} PUBLIC DISPLAY_${PANEL})

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
        src/touch.cpp
        src/leds.cpp
        src/pilbox.cpp
        src/plotter.cpp
        src/plotterview.cpp
        src/terminal.cpp
        src/hw_config.cpp
        src/my_descriptors.c
    )

    target_link_libraries(${EXE_TARGET} PRIVATE
        ${CORE_TARGET}
        hardware_i2c
        hardware_spi
        hardware_pwm
        hardware_gpio
        ${TRANSPORT_TARGET}
        pico_stdlib
        tinyusb_device          # ← replace pico_stdio_usb with this
        tinyusb_board
    #    pico_bootsel_via_double_reset
        # pico_cyw43_arch_none   # Pico 2 W: behövs för cyw43_arch_*
    )

    target_compile_definitions(${EXE_TARGET} PRIVATE
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR=0
    )

    # Generera header från .pio-filen
    pico_generate_pio_header(${EXE_TARGET}
        ${CMAKE_CURRENT_LIST_DIR}/src/hpil.pio
    )

    pico_enable_stdio_usb(${EXE_TARGET} 0)
    pico_enable_stdio_uart(${EXE_TARGET} 0)

    # Names every generated output (.uf2/.elf/.bin/.hex/.map) after
    # EXE_TARGET automatically -- this is what makes hipi_5_pico.uf2 /
    # hipi_7_pico.uf2 (etc.) come out named correctly, with no extra work.
    pico_add_extra_outputs(${EXE_TARGET})
endfunction()

message(STATUS "hipi: building Pico firmware for both panels (hipi_5_pico, hipi_7_pico)")
hipi_add_pico_targets(5INCH 5)
hipi_add_pico_targets(7INCH 7)

# ----------------------------------------------------------------------
# Optional: Linux spidev demo (off by default for Pico-focused work).
# Links against whichever panel's core library HIPI_DISPLAY_PANEL selects
# (both already exist, built above by hipi_add_pico_targets()).
# ----------------------------------------------------------------------
option(HIPI_BUILD_LINUX_EXAMPLE "Build the Linux spidev demo" OFF)
if(HIPI_BUILD_LINUX_EXAMPLE)
    message(STATUS "hipi: Linux demo targets DISPLAY_${HIPI_DISPLAY_PANEL}")
    add_executable(hipi_linux_demo
        src/LinuxSpiDevTransport.cpp
        src/linux_main.cpp
    )
    if(HIPI_DISPLAY_PANEL STREQUAL "7INCH")
        target_link_libraries(hipi_linux_demo PRIVATE hipi_core_7)
    else()
        target_link_libraries(hipi_linux_demo PRIVATE hipi_core_5)
    endif()
endif()
