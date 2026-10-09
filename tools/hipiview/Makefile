# hipiview -- HIPI's 7" display on the PC. Needs: g++, SDL2, zlib, and
# ffmpeg for recording.  On Ubuntu:
#   sudo apt install build-essential libsdl2-dev zlib1g-dev ffmpeg
#   make

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

SOURCES := hipiview.cpp lt7683_emu.cpp serial_port.cpp
HEADERS := lt7683_emu.hpp deframer.hpp png.hpp input.hpp serial_port.hpp ../../include/mirror_protocol.h

hipiview: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) $(SOURCES) -o $@ $(SDL_LIBS) -lz -pthread

# Windows, cross-compiled on Linux (MinGW-w64): one hipiview.exe with
# SDL2 and zlib built in. Needs the mingw SDL2 package and a mingw zlib
# (see README.md, "Windows"):
#   make windows SDL2_MINGW=/path/to/SDL2-2.x.y/x86_64-w64-mingw32 ZLIB_MINGW=/path/to/zlib
MINGW      ?= x86_64-w64-mingw32-g++
SDL2_MINGW ?= $(HOME)/mingw/SDL2/x86_64-w64-mingw32
ZLIB_MINGW ?= $(HOME)/mingw/zlib
windows: hipiview.exe
hipiview.exe: $(SOURCES) $(HEADERS)
	$(MINGW) -O2 -std=c++17 -Wall -Wextra -I$(SDL2_MINGW)/include -I$(ZLIB_MINGW)/include $(SOURCES) -o $@ \
	    -static -L$(SDL2_MINGW)/lib -L$(ZLIB_MINGW)/lib -lmingw32 -lSDL2main -lSDL2 -lz \
	    -lsetupapi -ladvapi32 -lwinmm -limm32 -lversion -lole32 -loleaut32 -luuid -lgdi32 -luser32 \
	    -lshell32 -lcfgmgr32 -mconsole

# Runs HIPI's real display driver, Screen and mirror code against an
# emulated chip, and checks that the mirror stream reproduces the screen
host_test: test/host_test.cpp lt7683_emu.cpp lt7683_emu.hpp ../../src/LT7683.cpp ../../src/Screen.cpp \
           ../../include/mirror_encoder.hpp ../../include/mirror_session.hpp
	$(CXX) $(CXXFLAGS) -DDISPLAY_7INCH -I../../include test/host_test.cpp lt7683_emu.cpp \
	    ../../src/LT7683.cpp ../../src/Screen.cpp -o $@ -lz

test: host_test
	./host_test

install: hipiview
	install -m 755 hipiview /usr/local/bin/

clean:
	rm -f hipiview hipiview.exe host_test link_sim *_viewer.png *_chip.png *.hvlog

.PHONY: test install clean windows

# The whole mirror over a lossy "USB": HIPI's real display_mirror.cpp
# (TinyUSB stood in by test/link_sim.cpp) talks to the real hipiview
# through a pseudo-terminal; the picture must come out right anyway.
#   ./link_sim [loss-probability] [seconds]
link_sim: test/link_sim.cpp hipiview lt7683_emu.cpp deframer.hpp ../../src/display_mirror.cpp \
          ../../include/mirror_encoder.hpp ../../include/mirror_session.hpp
	$(CXX) $(CXXFLAGS) -DDISPLAY_7INCH -Itest/stubs -I../../include test/link_sim.cpp lt7683_emu.cpp \
	    ../../src/display_mirror.cpp ../../src/LT7683.cpp ../../src/Screen.cpp -o $@ -lz -lutil
