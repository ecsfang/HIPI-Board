// bme280.h -- the Bosch BME280 (temperature, pressure and humidity), for
// I2C's "builtin bme280.T" / "bme280.P" / "bme280.H" (sdcard/i2c/chips.txt).
//
// The raw readings are turned into degrees, pascals and per cent with the
// chip's own calibration (registers 0x88..0x9F, 0xA1 and 0xE1..0xE7), using
// the integer formulas of Bosch's datasheet (BST-BME280-DS001, section
// 4.2.3). Temperature and pressure are calculated exactly as for the BMP280.
// It talks through I2cBus, so it runs on the PC too (tools/i2ctest).
#pragma once
#include "i2c_chips.h"
#include <cstdint>

namespace hipi {

// Raw readings to temperature (deg C), pressure (Pa) and humidity (%RH).
// `tp` is the 24 bytes from 0x88, `h1` the byte at 0xA1, `h` the 7 bytes
// from 0xE1; adcT and adcP are the 20-bit values, adcH the 16-bit one.
void bme280_compensate(const std::uint8_t tp[24], std::uint8_t h1, const std::uint8_t h[7], std::int32_t adcT,
                       std::int32_t adcP, std::int32_t adcH, double& tempC, double& pressurePa, double& humidity);

// One forced-mode measurement of all three (about 20 ms). False: the chip
// didn't answer, isn't a BME280 (ID 0x60) or didn't finish in time.
bool bme280_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa, double& humidity);

}  // namespace hipi
