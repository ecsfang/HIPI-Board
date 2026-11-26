// bme680.h -- the Bosch BME680 (and the BME688, which has the same chip ID
// and the same temperature, pressure and humidity), for I2C's
// "builtin bme680.T" / "bme680.P" / "bme680.H" (sdcard/i2c/chips.txt).
// The gas sensor is not read.
//
// The raw readings are turned into degrees, pascals and per cent with the
// chip's own calibration (registers 0x8A..0xA0 and 0xE1..0xEA), using the
// floating-point formulas of Bosch's datasheet (BST-BME680-DS001, section
// 3.3). It talks through I2cBus, so it runs on the PC too (tools/i2ctest).
#pragma once
#include "i2c_chips.h"
#include <cstdint>

namespace hipi {

// Raw readings to temperature (deg C), pressure (Pa) and humidity (%RH):
// `a` is the 23 bytes from 0x8A, `b` the 10 bytes from 0xE1; adcT and adcP
// are the 20-bit values, adcH the 16-bit one.
void bme680_compensate(const std::uint8_t a[23], const std::uint8_t b[10], std::int32_t adcT, std::int32_t adcP,
                       std::int32_t adcH, double& tempC, double& pressurePa, double& humidity);

// One forced-mode measurement (temperature, pressure and humidity, no gas;
// about 30 ms). False: the chip didn't answer, isn't a BME680/BME688 (ID
// 0x61) or didn't finish in time.
bool bme680_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa, double& humidity);

}  // namespace hipi
