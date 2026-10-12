# hipiview -- HIPI's 7" display on the PC: Linux, Windows (and macOS).
# Not part of the firmware build (that's the CMakeLists.txt at the top);
# build it on its own:
#
#   cmake -B build -S .            (in tools/hipiview)
#   cmake --build build
#
# Needs SDL2 and zlib (see README.md for each system). On Linux, the
# Makefile here does the same, plus the tests.
cmake_minimum_required(VERSION 3.16)
project(hipiview CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_executable(hipiview hipiview.cpp lt7683_emu.cpp serial_port.cpp)

# The version, from git (../../cmake/hipi_version.cmake): now, and again at
# every build; gen/hipiview_version.h only changes with the version
set(HIPIVIEW_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/gen/hipiview_version.h)
set(HIPIVIEW_VERSION_ARGS -DOUT=${HIPIVIEW_VERSION_HEADER} -DPREFIX=HIPIVIEW
                          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/../..
                          -P ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/hipi_version.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${HIPIVIEW_VERSION_ARGS})
add_custom_target(hipiview_version ALL
    COMMAND ${CMAKE_COMMAND} ${HIPIVIEW_VERSION_ARGS}
    BYPRODUCTS ${HIPIVIEW_VERSION_HEADER}
    VERBATIM)
add_dependencies(hipiview hipiview_version)
target_include_directories(hipiview PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(hipiview PRIVATE ZLIB::ZLIB Threads::Threads)

# SDL2's own CMake files name their targets SDL2::SDL2 (+ SDL2::SDL2main on
# Windows); older installs only set SDL2_INCLUDE_DIRS / SDL2_LIBRARIES
if(TARGET SDL2::SDL2main)
    target_link_libraries(hipiview PRIVATE SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2)
    target_link_libraries(hipiview PRIVATE SDL2::SDL2)
else()
    target_include_directories(hipiview PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(hipiview PRIVATE ${SDL2_LIBRARIES})
endif()

if(WIN32)
    # Finding the COM port (SetupAPI) and its name (registry)
    target_link_libraries(hipiview PRIVATE setupapi advapi32)
    if(MINGW)
        target_link_options(hipiview PRIVATE -mconsole)   # messages in the console window
    endif()
endif()

if(MSVC)
    target_compile_options(hipiview PRIVATE /W3 /utf-8)
    target_compile_definitions(hipiview PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(hipiview PRIVATE -Wall -Wextra)
endif()

install(TARGETS hipiview RUNTIME DESTINATION bin)
