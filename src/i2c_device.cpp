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
    std::uint8_t rxdata;
    for (int addr = 0x08; addr <= 0x77; ++addr) {
        // A single-byte read is the reliable way to probe for a response
        // on this hardware -- confirmed on real hardware that the
        // zero-length WRITE this used before doesn't actually work (some
        // devices apparently don't ACK a write with no data byte the same
        // way they ACK the address phase of a read). Ret >= 0 means the
        // address was acknowledged; PICO_ERROR_GENERIC (negative) means
        // nothing answered. The byte actually read back is discarded --
        // this is purely a presence probe, not a real register read.
        const int ret = i2c_read_blocking(bus, static_cast<std::uint8_t>(addr), &rxdata, 1, false);
        found[addr] = (ret >= 0);
    }
}
