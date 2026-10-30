# pixeltest -- TFPIXEL's pixel count, effects and current limit, on the PC:
# HIPI's real src/pixels.cpp with the real PicoLED library; the PIO and the
# clock are stood in (stubs/), and what goes out to the strip is checked.
#
#   make test
#
# Built with the address and undefined-behaviour sanitizers. (PicoLED's
# Particles frees its buffer with delete instead of delete[] -- harmless
# on the Pico, but the sanitizer would stop there, so that check is off.)
HIPI    := ../..
PICOLED := $(HIPI)/PicoLED
CXX     ?= g++
CXXFLAGS := -std=c++17 -g -O1 -Wall -fsanitize=address,undefined \
            -Istubs -I$(HIPI)/include -I$(PICOLED) -I$(PICOLED)/Effects
SRC := test.cpp $(HIPI)/src/pixels.cpp \
       $(PICOLED)/PicoLedTarget.cpp $(PICOLED)/PicoLedController.cpp $(PICOLED)/PicoLedEffect.cpp \
       $(PICOLED)/VirtualStrip.cpp $(PICOLED)/PioStrip.cpp $(PICOLED)/WS2812B.cpp \
       $(wildcard $(PICOLED)/Effects/*.cpp)

pixeltest: $(SRC) $(wildcard stubs/*.h stubs/*/*.h) $(HIPI)/include/pixels.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

# (only the verdicts shown -- but the test's own exit status counts: piped
# into grep alone, a failing test passed as long as grep found a line)
test: pixeltest
	ASAN_OPTIONS=alloc_dealloc_mismatch=0 ./pixeltest > pixeltest.log; r=$$?; grep -a "FAIL\|PASSED" pixeltest.log; exit $$r

clean:
	rm -f pixeltest pixeltest.log

.PHONY: test clean
