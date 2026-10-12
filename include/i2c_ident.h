// i2c_ident.h -- what is on the I2C bus: every address that answers, and
// for the ones HIPI knows, what chip it is and whether it works. Used by
// the Hardware test page (hwtest.h).
//
// The chips HIPI knows are described in text files: every .txt in i2c/ on
// the SD card (sdcard/i2c/chips.txt is the one that comes with HIPI, and
// its built-in copy is used without the folder) -- add a chip there, no
// new firmware needed. The format: i2c_chips.h and chips.txt itself.
//
// Identification reads: for a chip with an ID register (or values that
// can be checked) the check reads it -- the only thing written is the
// register number to read from, and never on 0x20-0x27 (a PCF8574 there
// would take it as output levels). Chips without checks are named as
// "probably ..." from their address.
#pragma once
#include <cstdint>
#include <string>

namespace hipi {

enum class I2cState : std::uint8_t {
    Ok,         // identified (ID register, sane reading)
    Fault,      // identified, but something's wrong (e.g. clock stopped)
    Probable,   // answers on an address this chip uses -- not confirmed
    Unknown,    // answers, nothing known about this address
    Missing,    // should be there (e.g. the touch panel), doesn't answer
};

struct I2cFound {
    std::uint8_t addr = 0;
    I2cState state = I2cState::Unknown;
    char name[20] = "";          // "DS3231", "Unknown"
    char what[28] = "";          // "real-time clock"
    char detail[40] = "";        // "12:34:56  23.8 C", "ID 0xEE"
    char file[24] = "";          // the description's file: "chips.txt", "built in", "" (unknown chip)
};

// Probes every address (0x08-0x77): found[a] = answered. Then identifies
// what answered (and lists the expected chips that didn't), at most `max`
// entries in address order. Returns the number of entries. Takes a few
// tens of milliseconds.
int i2c_identify(I2cFound* out, int max, bool found[128]);

// Reads the chip descriptions again (i2c/*.txt, else the built-in list);
// returns what was read, e.g. "27 descriptions  (SD card: i2c/chips.txt)". Lines it
// can't read go to the log.
std::string i2c_loadChips();

}  // namespace hipi
