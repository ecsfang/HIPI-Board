// rtc.h -- DS3231 real-time clock on the board's I2C bus (shared with the
// touch controller and the BMP280; address 0x68). Battery-backed, so the
// time survives power-off once it has been set.
#pragma once
#include "i2c_device.h"
#include <cstdint>

namespace hipi {

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

    // Checks the chip answers; present() afterwards
    bool begin(uint gpioSda, uint gpioScl);
    bool present() const { return present_; }
    // False if the chip lost its time (battery out, never set) -- then
    // the time it gives is meaningless until set()
    bool valid();

    bool read(DateTime& t);
    bool set(const DateTime& t);

private:
    bool present_ = false;
};

}  // namespace hipi
