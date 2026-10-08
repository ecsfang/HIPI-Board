#include "rtc.h"
#include <initializer_list>

namespace hipi {

namespace {
int fromBcd(std::uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
std::uint8_t toBcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }
constexpr std::uint8_t kRegSeconds = 0x00;
constexpr std::uint8_t kRegStatus  = 0x0F;
constexpr std::uint8_t kOscStopped = 0x80;   // DS3231 status: OSF, time not valid
constexpr std::uint8_t kClockHalt  = 0x80;   // DS1307 seconds: CH, oscillator halted
constexpr std::uint8_t kRegTempLsb = 0x12;   // DS3231: read-only, low 6 bits always 0;
                                             // DS1307: plain battery-backed RAM
}

int weekdayOf(int year, int month, int day) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

RtcChip CRtc::detectChip() {
#if HIPI_RTC_CHIP == 1
    return RtcChip::DS3231;
#elif HIPI_RTC_CHIP == 2
    return RtcChip::DS1307;
#else
    // Both chips have the time at 0x00..0x06, but register 0x12 differs:
    // on the DS3231 it is the read-only temperature LSB (bits 5..0 always
    // read 0, writes are ignored); on the DS1307 it is ordinary RAM. So
    // write two complementary patterns and see if they stick. The DS3231
    // is not changed by this, and 0x12 is unused RAM on the DS1307.
    bool ram = true;
    for (const std::uint8_t pattern : { std::uint8_t(0x2A), std::uint8_t(0x15) }) {
        std::uint8_t v = 0;
        if (!writeReg8(kRegTempLsb, pattern) || !readReg8(kRegTempLsb, v) || v != pattern) {
            ram = false;
            break;
        }
    }
    return ram ? RtcChip::DS1307 : RtcChip::DS3231;
#endif
}

const char* CRtc::chipName() const {
    switch (chip_) {
        case RtcChip::DS3231: return "DS3231";
        case RtcChip::DS1307: return "DS1307";
        default:              return "RTC";
    }
}

bool CRtc::begin(uint gpioSda, uint gpioScl) {
    initBus(gpioSda, gpioScl);
    std::uint8_t sec = 0;
    present_ = readReg8(kRegSeconds, sec);    // does anything answer at 0x68?
    chip_ = present_ ? detectChip() : RtcChip::None;
    return present_;
}

bool CRtc::valid() {
    if (!present_) return false;
    std::uint8_t v = 0;
    if (chip_ == RtcChip::DS1307) {
        if (!readReg8(kRegSeconds, v)) return false;
        return (v & kClockHalt) == 0;
    }
    if (!readReg8(kRegStatus, v)) return false;
    return (v & kOscStopped) == 0;
}

bool CRtc::read(DateTime& t) {
    std::uint8_t r[7];
    if (!present_ || !readReg(kRegSeconds, r, sizeof(r))) return false;
    t.second = fromBcd(r[0] & 0x7F);         // masks the DS1307's CH bit too
    t.minute = fromBcd(r[1] & 0x7F);
    if (r[2] & 0x40) {                       // 12-hour mode (not set by us)
        int h = fromBcd(r[2] & 0x1F) % 12;
        if (r[2] & 0x20) h += 12;
        t.hour = h;
    } else {
        t.hour = fromBcd(r[2] & 0x3F);
    }
    t.day = fromBcd(r[4] & 0x3F);
    t.month = fromBcd(r[5] & 0x1F);
    t.year = 2000 + fromBcd(r[6]);
    t.weekday = weekdayOf(t.year, t.month, t.day);
    return true;
}

bool CRtc::set(const DateTime& t) {
    if (!present_) return false;
    const std::uint8_t buf[8] = {
        kRegSeconds, toBcd(t.second), toBcd(t.minute), toBcd(t.hour),   // 24-hour mode
        static_cast<std::uint8_t>(weekdayOf(t.year, t.month, t.day) + 1),
        toBcd(t.day), toBcd(t.month), toBcd(t.year % 100) };
    if (i2c_write_blocking(bus_, address_, buf, sizeof(buf), false) != static_cast<int>(sizeof(buf)))
        return false;
    // DS1307: toBcd(second) has bit 7 clear, so the write above also
    // cleared CH and started the oscillator.
    if (chip_ == RtcChip::DS3231) {          // clear "oscillator stopped": time is valid now
        std::uint8_t status = 0;
        if (readReg8(kRegStatus, status)) writeReg8(kRegStatus, status & ~kOscStopped);
    }
    return true;
}

}  // namespace hipi
