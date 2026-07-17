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
# CMakeLists.txt for the hp82163_cpp project, structured to build under
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

project(hp82163_pico C CXX)

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
# Core library — RA8875 driver + HP82163 Screen class (platform-neutral).
# ----------------------------------------------------------------------
add_library(hp82163_core STATIC
    src/RA8875.cpp
    src/Screen.cpp
)
target_include_directories(hp82163_core PUBLIC include)

# ----------------------------------------------------------------------
# Pico-specific transport — links against pico-sdk's SPI/GPIO hw libs.
# ----------------------------------------------------------------------
add_library(hp82163_pico_transport STATIC
    src/PicoSpiTransport.cpp
)

## add_compile_definitions(CFG_TUD_CDC=2)
## add_compile_definitions(CFG_TUD_CDC_RX_BUFSIZE=256)
## add_compile_definitions(CFG_TUD_CDC_TX_BUFSIZE=256)

add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src build)

target_link_libraries(hp82163_pico_transport PUBLIC
    hp82163_core
    pico_stdlib
    hardware_spi
    hardware_gpio
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

target_include_directories(hp82163_pico_transport PUBLIC include)

# ----------------------------------------------------------------------
# Demo executable — flashed by the VS Code extension.
# ----------------------------------------------------------------------
add_executable(hp82163_pico
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
    src/hw_config.cpp
    src/my_descriptors.c
)

# Lägg till detta EFTER pico_sdk_init() och EFTER add_executable(...).
# Det tvingar tinyusb_device att omkompilera med rätt defines.

# tinyusb_device är ett INTERFACE-bibliotek som vi ger COMPILE_DEFINITIONS via.
# (Det skapas av SDK:ns pico_enable_* -makron. Vi når det via target name.)

## set_target_properties(tinyusb_device PROPERTIES
##     INTERFACE_COMPILE_DEFINITIONS "CFG_TUD_CDC=2;CFG_TUD_CDC_RX_BUFSIZE=256;CFG_TUD_CDC_TX_BUFSIZE=256"
## )

target_link_libraries(hp82163_pico PRIVATE
    hp82163_core
    hardware_i2c
    hardware_spi
    hardware_pwm
    hardware_gpio
    hp82163_pico_transport
    pico_stdlib
    tinyusb_device          # ← replace pico_stdio_usb with this
    tinyusb_board
#    pico_bootsel_via_double_reset 
    # pico_cyw43_arch_none   # Pico 2 W: behövs för cyw43_arch_*
)

target_compile_definitions(hp82163_pico PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR=0
)

# 3. Generera header från .pio-filen
pico_generate_pio_header(hp82163_pico
    ${CMAKE_CURRENT_LIST_DIR}/src/hpil.pio
)


pico_enable_stdio_usb(hp82163_pico 0)
pico_enable_stdio_uart(hp82163_pico 0)

pico_add_extra_outputs(hp82163_pico)

# ----------------------------------------------------------------------
# Optional: Linux spidev demo (off by default for Pico-focused work).
# ----------------------------------------------------------------------
option(HP82163_BUILD_LINUX_EXAMPLE "Build the Linux spidev demo" OFF)
if(HP82163_BUILD_LINUX_EXAMPLE)
    add_executable(hp82163_linux_demo
        src/LinuxSpiDevTransport.cpp
        src/linux_main.cpp
    )
    target_link_libraries(hp82163_linux_demo PRIVATE hp82163_core)
endif()
