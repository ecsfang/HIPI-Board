# i2ctest -- the I2C chip descriptions on the PC (src/i2c_chips.cpp): the
# file format, checked with sdcard/i2c/chips.txt itself, and identifying
# chips on a made-up bus, the measurements, TFI2C's commands and the
# device itself on the loop (printertest's stand-ins for the Pico SDK).
#
#   make test
HIPI    := ../..
CXX     ?= g++
CXXFLAGS := -std=c++17 -g -O1 -Wall -Wextra -fsanitize=address,undefined -I../printertest/stubs -I$(HIPI)/include
SRC := test.cpp $(HIPI)/src/i2c_chips.cpp $(HIPI)/src/i2c_command.cpp $(HIPI)/src/ili2c.cpp $(HIPI)/src/device.cpp $(HIPI)/src/vl53l0x.cpp

i2ctest: $(SRC) $(HIPI)/include/i2c_chips.h $(HIPI)/include/i2c_ident.h $(HIPI)/include/i2c_command.h $(HIPI)/include/vl53l0x.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

test: i2ctest
	./i2ctest $(HIPI)/sdcard/i2c/chips.txt

clean:
	rm -f i2ctest

.PHONY: test clean
