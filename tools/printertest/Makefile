# printertest -- TFPRINT (HP 82162A, src/printer.cpp) on the PC: its HP-IL
# answers (status bytes, service request, identification) and the text it
# takes, with HIPI's real device code. The clock is stood in (stubs/).
#
#   make test
HIPI    := ../..
CXX     ?= g++
CXXFLAGS := -std=c++17 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
            -Istubs -I$(HIPI)/include
SRC := test.cpp $(HIPI)/src/printer.cpp $(HIPI)/src/printer_font.cpp $(HIPI)/src/device.cpp

printertest: $(SRC) $(wildcard stubs/*.h stubs/*/*.h) $(HIPI)/include/printer.h $(HIPI)/include/hpil.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

test: printertest
	./printertest

clean:
	rm -f printertest

.PHONY: test clean
