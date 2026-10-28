// rtc.h -- real-time clock on the board's I2C bus (shared with the touch
// controller and the BMP280): a DS3231 or DS1307 at 0x68, or a PCF85063A
// at 0x51. Battery-backed, so the time survives power-off once it has been
// set.
//
// Which chip is fitted is detected by CRtc::begin() -- at startup, and again
// by clock.cpp while none answers (a chip plugged in later). 0x68 is looked
// at first, then 0x51. To skip the detection and force a chip, build with
// -DHIPI_RTC_CHIP=1 (DS3231), 2 (DS1307) or 3 (PCF85063A); the default 0
// means auto-detect.
#pragma once
#include "i2c_device.h"
#include <cstdint>

#ifndef HIPI_RTC_CHIP
#define HIPI_RTC_CHIP 0     // 0 = auto-detect, 1 = DS3231, 2 = DS1307, 3 = PCF85063A
#endif

namespace hipi {

enum class RtcChip { None, DS3231, DS1307, PCF85063A };

constexpr std::uint8_t kRtcAddrDs = 0x68;        // DS3231, DS1307
constexpr std::uint8_t kRtcAddrPcf = 0x51;       // PCF85063A

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

// The PCF85063A's time registers 0x04..0x0A (seconds .. years) look like
// one: valid BCD, weekday 0..6, month 1..12. Tells it from a PCF8563 at
// the same address, whose year sits where the PCF85063A has the weekday.
bool looksLikePcf85063(const std::uint8_t r[7]);

// A PCF85063A even with garbled registers: lo = registers 0x00..0x11, hi =
// the same read from 0x20 (its register address has 5 bits, so they must
// be equal), and every bit it doesn't have reads 0
constexpr int kPcfRegs = 0x12;
bool pcf85063Fingerprint(const std::uint8_t lo[kPcfRegs], const std::uint8_t hi[kPcfRegs]);

class CRtc : public CI2CDevice {
public:
    explicit CRtc(i2c_inst_t* bus) : CI2CDevice(bus, kRtcAddrDs) {}

    // Looks for a chip and which type it is; present() afterwards
    bool begin(uint gpioSda, uint gpioScl);
    bool present() const { return present_; }
    RtcChip chip() const { return chip_; }
    std::uint8_t address() const { return address_; }
    const char* chipName() const;
    // False if the chip lost its time (DS3231: oscillator stopped flag;
    // DS1307: clock-halt bit; PCF85063A: OS flag, STOP or EXT_TEST, or
    // garbled time registers) -- then the
    // time it gives is meaningless until set()
    bool valid();

    bool read(DateTime& t);
    bool set(const DateTime& t);
    // Setting it to the start of a second (the time from the PC,
    // mirror_clock.h): hold() writes the time with the clock stopped,
    // release() lets it run, and its first tick comes startLeadUs() after
    // that. The PCF85063A needs this: writing its seconds doesn't restart
    // its count of the second (only STOP does -- the first tick then comes
    // 0.507813 .. 0.507935 s after STOP is released, datasheet 7.2.1.2). The
    // DS3231/DS1307 restart it whenever the seconds are written: hold() is
    // set(), release() nothing, the lead 0.
    bool hold(const DateTime& t);
    bool release();
    std::uint32_t startLeadUs() const { return chip_ == RtcChip::PCF85063A ? 507874u : 0u; }

    // After a begin() that found no chip: did something answer on 0x51
    // all the same (then regs = its registers 0x00..0x0A, for the log)?
    bool answeredOnPcfAddress(std::uint8_t regs[11]) const {
        for (int i = 0; i < 11; ++i) regs[i] = pcfRegs_[i];
        return pcfAnswered_;
    }

private:
    // Tells the two 0x68 chips apart (both answer there)
    RtcChip detectDsChip();
    bool findPcf();

    bool present_ = false;
    RtcChip chip_ = RtcChip::None;
    bool pcfAnswered_ = false;
    std::uint8_t pcfRegs_[11] = {};
};

}  // namespace hipi
