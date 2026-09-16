#include "i2c_device.h"

bool CI2CBus::i2c0Ready_ = false;
bool CI2CBus::i2c1Ready_ = false;

void CI2CBus::markInitialized(i2c_inst_t* bus) {
    if (bus == i2c0) i2c0Ready_ = true;
    else if (bus == i2c1) i2c1Ready_ = true;
}

bool CI2CBus::isInitialized(i2c_inst_t* bus) {
    if (bus == i2c0) return i2c0Ready_;
    if (bus == i2c1) return i2c1Ready_;
    return false;
}

void CI2CBus::scan(i2c_inst_t* bus, bool found[128]) {
    uint8_t rxdata;
    for (int addr = 0x08; addr <= 0x77; ++addr) {
        // A zero-length write is the standard, minimal-risk way to probe
        // for a response -- it's just the address byte itself (with the
        // R/W bit clear) followed immediately by a stop condition, no
        // actual register/data byte sent, so it can't accidentally write
        // anything meaningful even to a device that DOES respond. Ret
        // >= 0 means the address was acknowledged; PICO_ERROR_GENERIC
        // (negative) means nothing answered.
        const int ret = i2c_read_blocking(bus, static_cast<std::uint8_t>(addr), &rxdata, 1, false);
        found[addr] = (ret >= 0);
    }
}
