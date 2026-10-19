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

# The version, from git (../../cmake/hipi_version.cmake, run every time; it
# only rewrites gen/hipiview_version.h when the version changes). Without
# cmake: "unknown".
VERSION_H := gen/hipiview_version.h
$(VERSION_H): FORCE
	@if command -v cmake >/dev/null 2>&1; then \
	    cmake -DOUT=$(CURDIR)/$@ -DPREFIX=HIPIVIEW -P ../../cmake/hipi_version.cmake; \
	elif [ ! -f $@ ]; then \
	    mkdir -p gen && printf '#pragma once\n#define HIPIVIEW_VERSION_TEXT "unknown"\n#define HIPIVIEW_VERSION_FULL "unknown"\n' > $@; \
	fi
.PHONY: FORCE
FORCE:

hipiview: $(SOURCES) $(HEADERS) $(VERSION_H)
	$(CXX) $(CXXFLAGS) -Igen $(SDL_CFLAGS) $(SOURCES) -o $@ $(SDL_LIBS) -lz -pthread

# Windows, cross-compiled on Linux (MinGW-w64): one hipiview.exe with
# SDL2 and zlib built in. Needs the mingw SDL2 package and a mingw zlib
# (see README.md, "Windows"):
#   make windows SDL2_MINGW=/path/to/SDL2-2.x.y/x86_64-w64-mingw32 ZLIB_MINGW=/path/to/zlib
MINGW      ?= x86_64-w64-mingw32-g++
SDL2_MINGW ?= $(HOME)/mingw/SDL2/x86_64-w64-mingw32
ZLIB_MINGW ?= $(HOME)/mingw/zlib
windows: hipiview.exe
hipiview.exe: $(SOURCES) $(HEADERS) $(VERSION_H)
	$(MINGW) -O2 -std=c++17 -Wall -Wextra -Igen -I$(SDL2_MINGW)/include -I$(ZLIB_MINGW)/include $(SOURCES) -o $@ \
	    -static -L$(SDL2_MINGW)/lib -L$(ZLIB_MINGW)/lib -lmingw32 -lSDL2main -lSDL2 -lz \
	    -lsetupapi -ladvapi32 -lwinmm -limm32 -lversion -lole32 -loleaut32 -luuid -lgdi32 -luser32 \
	    -lshell32 -lcfgmgr32 -mconsole

# Runs HIPI's real display driver, Screen and mirror code against an
# emulated chip, and checks that the mirror stream reproduces the screen
host_test: test/host_test.cpp lt7683_emu.cpp lt7683_emu.hpp ../../src/LT7683.cpp ../../src/Screen.cpp \
           ../../include/mirror_encoder.hpp ../../include/mirror_session.hpp
	$(CXX) $(CXXFLAGS) -I../../include test/host_test.cpp lt7683_emu.cpp \
	    ../../src/LT7683.cpp ../../src/Screen.cpp -o $@ -lz

# The Printer view as HIPI draws it (real plotterview.cpp, printer.cpp,
# BMP loader, LT7683 driver) with the pictures from resources/ -- saves
# printer_view_*.png to look at
PV_SRC := test/printer_view_test.cpp lt7683_emu.cpp ../../src/LT7683.cpp ../../src/Screen.cpp \
          ../../src/plotterview.cpp ../../src/printer.cpp ../../src/printer_font.cpp \
          ../../src/plotter.cpp ../../src/device.cpp ../../src/video80.cpp \
          ../../src/hwtest.cpp test/hwio_stub.cpp ../../src/mediamap.cpp ../../src/lif_create.cpp \
          ../../src/printout.cpp ../../src/hpil_signals.cpp ../../src/hpil.cpp
printer_view_test: $(PV_SRC) $(wildcard test/pvstubs/*.h test/pvstubs/*/*.h) ../../include/printer.h ../../include/plotterview.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -Itest -I../../include $(PV_SRC) -o $@ -lz

# TFDISPLAY as an HP 92198 / MC00701A: the real Video80 screen and the
# display device against the emulated chip -- saves video80_*.png to look at
V80_SRC := test/video80_view_test.cpp lt7683_emu.cpp ../../src/LT7683.cpp ../../src/Screen.cpp \
           ../../src/video80.cpp ../../src/display.cpp ../../src/device.cpp
video80_view_test: $(V80_SRC) ../../include/video80.h ../../include/display.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -I../../include $(V80_SRC) -o $@ -lz

# The Hardware test page as HIPI draws it (a stand-in for the LEDs,
# pixels and I2C bus) -- saves hwtest_*.png
HW_SRC := test/hwtest_view_test.cpp $(filter-out test/printer_view_test.cpp,$(PV_SRC))
hwtest_view_test: $(HW_SRC) test/hwio_stub.h ../../include/hwtest.h ../../include/hwtest_io.h ../../include/i2c_ident.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -Itest -I../../include $(HW_SRC) -o $@ -lz

# What the demo shows (demo_content.cpp) -- saves demo_*.png
DEMO_SRC := test/demo_view_test.cpp ../../src/demo_content.cpp ../../src/logo_image.cpp \
            $(filter-out test/printer_view_test.cpp,$(PV_SRC))
demo_view_test: $(DEMO_SRC) ../../include/demo_content.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -Itest -I../../include $(DEMO_SRC) -o $@ -lz

# The demo itself, a whole round in fake time -- saves demo_run_*.png
RUN_SRC := test/demo_run_test.cpp ../../src/demo.cpp ../../src/display.cpp \
           $(filter-out test/demo_view_test.cpp,$(DEMO_SRC))
demo_run_test: $(RUN_SRC) ../../include/demo.h ../../include/demo_content.h $(wildcard test/pvstubs/*.h test/demostubs/*.hpp)
	$(CXX) $(CXXFLAGS) -Itest/demostubs -Itest/pvstubs -Itest -I../../include $(RUN_SRC) -o $@ -lz

# File picker -> Create media...: the dialogs and the LIF image writer
# (a folder as the SD card) -- saves media_*.png
MEDIA_SRC := test/media_create_test.cpp lt7683_emu.cpp ../../src/LT7683.cpp ../../src/media_create_ui.cpp \
             ../../src/lif_create.cpp ../../src/glyph_font.cpp ../../src/menu_font_title.cpp
media_create_test: $(MEDIA_SRC) ../../include/media_create_ui.h ../../include/lif_create.h ../../include/menu_frame.h test/pvstubs/ff.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -Itest -I../../include $(MEDIA_SRC) -o $@ -lz

# The Media map view, on LIF images made by the test -- saves mediamap_*.png
MAP_SRC := test/mediamap_test.cpp lt7683_emu.cpp ../../src/LT7683.cpp ../../src/mediamap.cpp ../../src/lif_create.cpp
mediamap_test: $(MAP_SRC) ../../include/mediamap.h ../../include/lif_create.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -Itest -I../../include $(MAP_SRC) -o $@ -lz

# The HP-IL DEVICES list (devicelist.cpp) -- saves devicelist_*.png
DL_SRC := test/devicelist_test.cpp lt7683_emu.cpp ../../src/LT7683.cpp ../../src/devicelist.cpp ../../src/glyph_font.cpp ../../src/menu_font_title.cpp
devicelist_test: $(DL_SRC) ../../include/devicelist.h ../../include/device_info.h ../../include/menu_frame.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -Itest -I../../include $(DL_SRC) -o $@ -lz

# The HP-GL parser (plotter.cpp), no display
PLOT_SRC := test/plotter_test.cpp ../../src/plotter.cpp ../../src/device.cpp ../../src/hpil.cpp
plotter_test: $(PLOT_SRC) ../../include/plotter.h
	$(CXX) $(CXXFLAGS) -Itest/pvstubs -I../../include $(PLOT_SRC) -o $@

# The loop map, fed made-up HP-41 traffic -- saves loopmap_*.png
LM_SRC := test/loopmap_test.cpp lt7683_emu.cpp ../../src/LT7683.cpp ../../src/loopmap.cpp ../../src/device.cpp ../../src/hpil.cpp
loopmap_test: $(LM_SRC) ../../include/loopmap.h $(wildcard test/lmstubs/*.h)
	$(CXX) $(CXXFLAGS) -Itest/lmstubs -Itest/pvstubs -I../../include $(LM_SRC) -o $@ -lz

# The real-time clock chips (rtc.cpp, rtc_probe.cpp, the chip list's
# entries) against made-up chips: DS3231, DS1307, PCF85063A, PCF8563
RTC_SRC := test/rtc_test.cpp ../../src/rtc.cpp ../../src/rtc_probe.cpp ../../src/i2c_device.cpp ../../src/i2c_chips.cpp
rtc_test: $(RTC_SRC) ../../include/rtc.h ../../include/rtc_probe.h ../../sdcard/i2c/chips.txt $(wildcard test/rtcstubs/*/*.h)
	$(CXX) $(CXXFLAGS) -Itest/rtcstubs -I../../include $(RTC_SRC) -o $@

test: host_test printer_view_test video80_view_test hwtest_view_test demo_view_test demo_run_test media_create_test mediamap_test loopmap_test plotter_test devicelist_test rtc_test
	./host_test
	./printer_view_test
	./video80_view_test
	./hwtest_view_test
	./demo_view_test
	./demo_run_test
	./media_create_test
	./mediamap_test
	./loopmap_test
	./plotter_test
	./devicelist_test
	./rtc_test

install: hipiview
	install -m 755 hipiview /usr/local/bin/

clean:
	rm -rf gen
	rm -f hipiview hipiview.exe host_test link_sim printer_view_test video80_view_test video80_*.png hwtest_view_test hwtest_*.png demo_view_test demo_run_test demo_*.png media_create_test media_*.png mediamap_test mediamap_*.png loopmap_test loopmap_*.png plotter_test devicelist_test devicelist_*.png rtc_test *_viewer.png *_chip.png printer_view_*.png *.hvlog

.PHONY: test install clean windows

# The whole mirror over a lossy "USB": HIPI's real display_mirror.cpp
# (TinyUSB stood in by test/link_sim.cpp) talks to the real hipiview
# through a pseudo-terminal; the picture must come out right anyway.
#   ./link_sim [loss-probability] [seconds]
link_sim: test/link_sim.cpp hipiview lt7683_emu.cpp deframer.hpp ../../src/display_mirror.cpp \
          ../../include/mirror_encoder.hpp ../../include/mirror_session.hpp
	$(CXX) $(CXXFLAGS) -Itest/stubs -I../../include test/link_sim.cpp lt7683_emu.cpp \
	    ../../src/display_mirror.cpp ../../src/LT7683.cpp ../../src/Screen.cpp -o $@ -lz -lutil
