// bmp390.h -- the Bosch BMP390 (and BMP388) pressure sensor, for I2C's
// "builtin bmp390.T" / "builtin bmp390.P" (sdcard/i2c/chips.txt).
//
// The chip's raw readings are turned into degrees and pascals with 21 bytes
// of calibration data from the chip's own memory (register 0x31), using the
// floating-point formulas of Bosch's datasheet (BST-BMP390-DS002, section
// 8). It talks through I2cBus, so it runs on the PC too (tools/i2ctest).
#pragma once
#include "i2c_chips.h"
#include <cstdint>

namespace hipi {

// Raw readings to temperature (deg C) and pressure (Pa): `calib` is the 21
// bytes from register 0x31, `rawT` and `rawP` the 24-bit values.
void bmp390_compensate(const std::uint8_t calib[21], std::uint32_t rawT, std::uint32_t rawP, double& tempC,
                       double& pressurePa);

// One forced-mode measurement (about 20 ms: pressure x8, temperature x1).
// False: the chip didn't answer, isn't a BMP390/BMP388 (ID 0x60/0x50), or
// didn't finish in time.
bool bmp390_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa);

}  // namespace hipi
