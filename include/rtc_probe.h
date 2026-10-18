// rtc_probe.h -- the clock chips' checks for the I2C chip list ("probe
// clock", "probe pcf85063" in i2c/chips.txt): the Hardware test page and
// TFI2C's "I". Only reads; the chip drivers themselves are in rtc.h.
#pragma once
#include "i2c_chips.h"
#include <cstdint>
#include <string>

namespace hipi {

// DS3231 / DS1307 (0x68): valid BCD time, ticking; the DS3231 also gives
// its temperature and the "oscillator stopped" flag (time lost)
I2cVerdict rtc_probeDs(I2cBus& bus, std::uint8_t addr, std::string& name, std::string& detail);

// PCF85063A (0x51): the time registers look like it (not a PCF8563 or an
// EEPROM there), ticking; time lost = the OS flag, STOP or EXT_TEST. With
// garbled registers it's still known by its fingerprint (rtc.h): fault
I2cVerdict rtc_probePcf85063(I2cBus& bus, std::uint8_t addr, std::string& name, std::string& detail);

}  // namespace hipi
