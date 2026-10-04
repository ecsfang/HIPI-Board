#include "rtc.h"

namespace hipi {

namespace {
int fromBcd(std::uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
std::uint8_t toBcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }
constexpr std::uint8_t kRegSeconds = 0x00;
constexpr std::uint8_t kRegControl = 0x0E;
constexpr std::uint8_t kRegStatus  = 0x0F;
constexpr std::uint8_t kOscStopped = 0x80;   // status: OSF, time not valid
}

int weekdayOf(int year, int month, int day) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

bool CRtc::begin(uint gpioSda, uint gpioScl) {
    initBus(gpioSda, gpioScl);
    std::uint8_t ctrl = 0;
    present_ = readReg8(kRegControl, ctrl);
    return present_;
}

bool CRtc::valid() {
    std::uint8_t status = 0;
    if (!present_ || !readReg8(kRegStatus, status)) return false;
    return (status & kOscStopped) == 0;
}

bool CRtc::read(DateTime& t) {
    std::uint8_t r[7];
    if (!present_ || !readReg(kRegSeconds, r, sizeof(r))) return false;
    t.second = fromBcd(r[0] & 0x7F);
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
    std::uint8_t status = 0;                 // clear "oscillator stopped": time is valid now
    if (readReg8(kRegStatus, status)) writeReg8(kRegStatus, status & ~kOscStopped);
    return true;
}

}  // namespace hipi
