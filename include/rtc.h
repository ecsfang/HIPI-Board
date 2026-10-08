// rtc.h -- DS3231 or DS1307 real-time clock on the board's I2C bus (shared
// with the touch controller and the BMP280; both chips use address 0x68).
// Battery-backed, so the time survives power-off once it has been set.
//
// Which chip is fitted is detected at startup (CRtc::begin()). To skip the
// detection and force a chip, build with -DHIPI_RTC_CHIP=1 (DS3231) or
// -DHIPI_RTC_CHIP=2 (DS1307); the default 0 means auto-detect.
#pragma once
#include "i2c_device.h"
#include <cstdint>

#ifndef HIPI_RTC_CHIP
#define HIPI_RTC_CHIP 0     // 0 = auto-detect, 1 = DS3231, 2 = DS1307
#endif

namespace hipi {

enum class RtcChip { None, DS3231, DS1307 };

struct DateTime {
    int year = 2000;    // 2000..2099
    int month = 1;      // 1..12
    int day = 1;        // 1..31
    int hour = 0;       // 0..23
    int minute = 0;
    int second = 0;
    int weekday = 6;    // 0 = Sunday .. 6 = Saturday (computed from the date)
};

// Weekday of a date, 0 = Sunday (Sakamoto's method)
int weekdayOf(int year, int month, int day);

class CRtc : public CI2CDevice {
public:
    explicit CRtc(i2c_inst_t* bus, std::uint8_t address = 0x68) : CI2CDevice(bus, address) {}

    // Checks the chip answers and which type it is; present() afterwards
    bool begin(uint gpioSda, uint gpioScl);
    bool present() const { return present_; }
    RtcChip chip() const { return chip_; }
    const char* chipName() const;
    // False if the chip lost its time (DS3231: oscillator stopped flag;
    // DS1307: clock-halt bit) -- then the time it gives is meaningless
    // until set()
    bool valid();

    bool read(DateTime& t);
    bool set(const DateTime& t);

private:
    // Tells the two chips apart (both answer at 0x68)
    RtcChip detectChip();

    bool present_ = false;
    RtcChip chip_ = RtcChip::None;
};

}  // namespace hipi
