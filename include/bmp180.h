// bmp180.h -- the Bosch BMP180 (and its predecessor BMP085, which is
// calculated the same way), for I2C's "builtin bmp180.T" / "bmp180.P"
// (sdcard/i2c/chips.txt).
//
// The raw readings are turned into degrees and pascals with 11 calibration
// words from the chip's own memory (registers 0xAA..0xBF), using the integer
// algorithm of Bosch's datasheet (BST-BMP180-DS000, figure 4). It talks
// through I2cBus, so it runs on the PC too (tools/i2ctest).
#pragma once
#include "i2c_chips.h"
#include <cstdint>

namespace hipi {

// Raw readings to temperature (in 0.1 deg C) and pressure (Pa): `calib` is
// the 22 bytes from register 0xAA, `ut` the raw temperature, `up` the raw
// pressure (already shifted right by 8-oss) and `oss` its oversampling (0..3).
void bmp180_compensate(const std::uint8_t calib[22], std::int32_t ut, std::int32_t up, int oss, std::int32_t& temp01C,
                       std::int32_t& pressurePa);

// One temperature and one pressure measurement (about 30 ms in all, the
// pressure at the highest oversampling). False: the chip didn't answer, isn't
// a BMP180/BMP085 (ID 0x55) or its calibration data is not readable (a word
// that is 0 or 0xFFFF).
bool bmp180_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa);

}  // namespace hipi
