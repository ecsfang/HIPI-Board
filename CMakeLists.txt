cmake_minimum_required(VERSION 3.16)
project(hipi_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ----------------------------------------------------------------------
# Core library
# ----------------------------------------------------------------------
add_library(hipi_core
    src/RA8875.cpp
    src/Screen.cpp
)
target_include_directories(hipi_core PUBLIC include)

# ----------------------------------------------------------------------
# Linux spidev example
# ----------------------------------------------------------------------
option(HIPI_BUILD_LINUX_EXAMPLE "Build the Linux spidev example" ON)
if(HIPI_BUILD_LINUX_EXAMPLE)
    add_executable(hipi_linux_demo
        examples/main.cpp
        src/LinuxSpiDevTransport.cpp
    )
    target_include_directories(hipi_linux_demo PRIVATE include)
    target_link_libraries(hipi_linux_demo PRIVATE hipi_core)
endif()

# ----------------------------------------------------------------------
# Raspberry Pico 2 (RP2350) — requires pico-sdk
# ----------------------------------------------------------------------
option(HIPI_BUILD_PICO_EXAMPLE "Build the Raspberry Pico 2 example" OFF)
if(HIPI_BUILD_PICO_EXAMPLE)
    # Initialize the pico-sdk from the parent project if not already done.
    if(NOT PICO_SDK_PATH AND NOT DEFINED PICO_SDK_FETCH_FROM_GIT)
        message(FATAL_ERROR
            "HIPI_BUILD_PICO_EXAMPLE is ON but pico-sdk is not available. "
            "Set PICO_SDK_PATH or build this target from a pico project tree.")
    endif()

    if(NOT TARGET pico_stdlib)
        include(pico_sdk_import.cmake)
        pico_sdk_init()
    endif()

    add_library(hipi_pico_transport STATIC src/PicoSpiTransport.cpp)
    target_include_directories(hipi_pico_transport PUBLIC include)
    target_link_libraries(hipi_pico_transport PUBLIC
        pico_stdlib hardware_spi hardware_gpio
    )

    add_executable(hipi_pico_demo
        examples/pico_main.cpp
    )
    target_link_libraries(hipi_pico_demo
        PRIVATE
            hipi_core
            hipi_pico_transport
    )
    pico_enable_stdio_usb(hipi_pico_demo 1)
    pico_enable_stdio_uart(hipi_pico_demo 0)

    # Produce UF2 for drag-and-drop flashing.
    pico_add_extra_outputs(hipi_pico_demo)
endif()

# ----------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------
install(TARGETS hipi_core ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
