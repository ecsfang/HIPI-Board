// vl53l0x.h -- the VL53L0X time-of-flight distance sensor, for TFI2C's
// "builtin vl53l0x.D" (sdcard/i2c/chips.txt): HIPI does ST's whole set-up
// in the background (reference SPADs, tuning, timing, calibration), so the
// HP-41 only asks for a distance ("MD").
//
// The sequence is the one in ST's VL53L0X API, in the compact form of
// Pololu's VL53L0X Arduino library. It talks through I2cBus, so it runs
// on the PC too (tools/i2ctest, with a made-up chip).
#pragma once
#include "i2c_chips.h"
#include <cstdint>

namespace hipi {

// One distance in millimetres (a single measurement, about 33 ms). The
// set-up is done the first time, and again when the chip has lost it (it
// was unplugged, or switched off). 8190 or more: nothing in range. False:
// the chip didn't answer, or didn't finish in time.
bool vl53l0x_readMm(I2cBus& bus, std::uint8_t addr, double& mm);

// Forget every set-up (the next reading sets the chip up again)
void vl53l0x_reset();

}  // namespace hipi
