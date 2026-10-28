#include "rtc.h"
#include <initializer_list>

namespace hipi {

namespace {
int fromBcd(std::uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
std::uint8_t toBcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }
bool isBcd(std::uint8_t v, int lo, int hi) { return (v & 0x0F) <= 9 && fromBcd(v) >= lo && fromBcd(v) <= hi; }

// DS3231 / DS1307 (0x68)
constexpr std::uint8_t kRegSeconds = 0x00;
constexpr std::uint8_t kRegStatus  = 0x0F;
constexpr std::uint8_t kOscStopped = 0x80;   // DS3231 status: OSF, time not valid
constexpr std::uint8_t kClockHalt  = 0x80;   // DS1307 seconds: CH, oscillator halted
constexpr std::uint8_t kRegTempLsb = 0x12;   // DS3231: read-only, low 6 bits always 0;
                                             // DS1307: plain battery-backed RAM
// PCF85063A (0x51)
constexpr std::uint8_t kPcfControl1 = 0x00;
constexpr std::uint8_t kPcfStop     = 0x20;  // Control_1: STOP, the clock is halted
constexpr std::uint8_t kPcf12h      = 0x02;  // Control_1: 12_24, 12-hour mode
constexpr std::uint8_t kPcfExtTest  = 0x80;  // Control_1: EXT_TEST, clock from outside
constexpr std::uint8_t kPcfSoftReset = 0x58; // to Control_1: software reset
constexpr std::uint8_t kPcfSeconds  = 0x04;  // seconds, minutes, hours, days, weekdays, months, years
constexpr std::uint8_t kPcfOscStop  = 0x80;  // seconds: OS, oscillator stopped -- time not valid

// Hours register, 12-hour form (bit 5 = PM, 1..12) -> 0..23
int hour12(std::uint8_t v) {
    int h = fromBcd(v & 0x1F) % 12;
    if (v & 0x20) h += 12;
    return h;
}
}  // namespace

int weekdayOf(int year, int month, int day) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

bool looksLikePcf85063(const std::uint8_t r[7]) {
    // r: seconds, minutes, hours, days, weekdays, months, years. Hours in
    // either form: 24-hour 0..23, or 12-hour (bit 5 = PM) 1..12
    const bool hours = isBcd(r[2] & 0x3F, 0, 23) || isBcd(r[2] & 0x1F, 1, 12);
    return isBcd(r[0] & 0x7F, 0, 59) && isBcd(r[1] & 0x7F, 0, 59) && hours &&
           isBcd(r[3] & 0x3F, 1, 31) && (r[4] & 0x07) <= 6 && (r[4] & 0xF8) == 0 &&
           isBcd(r[5] & 0x1F, 1, 12) && isBcd(r[6], 0, 99);
}

bool pcf85063Fingerprint(const std::uint8_t lo[kPcfRegs], const std::uint8_t hi[kPcfRegs]) {
    // Its register address has 5 bits: 0x20.. reads 0x00.. again
    for (int i = 0; i < kPcfRegs; ++i)
        if (lo[i] != hi[i]) return false;
    // Bits it doesn't have read 0 (datasheet table 5: "-")
    static const std::uint8_t kUnused[kPcfRegs] = {
        0x48, 0x00, 0x00, 0x00, 0x00, 0x80, 0xC0, 0xC0, 0xF8,   // Control_1 .. Weekdays
        0xE0, 0x00, 0x00, 0x00, 0x40, 0x40, 0x78, 0x00, 0xE0 }; // Months .. Timer_mode
    for (int i = 0; i < kPcfRegs; ++i)
        if (lo[i] & kUnused[i]) return false;
    return true;
}

RtcChip CRtc::detectDsChip() {
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

// Something at 0x51 with the PCF85063A's time registers? (Reads only: an
// EEPROM or a PCF8563 there is left alone.)
// Something at 0x51 that is a PCF85063A? Its time registers look like
// one -- or, when they don't (registers garbled at power-up: datasheet
// 8.2.1.3; a module plugged in while powered), its fingerprint does
// (pcf85063Fingerprint()). Only reads: an EEPROM or a PCF8563 there is
// left alone.
bool CRtc::findPcf() {
    address_ = kRtcAddrPcf;
    std::uint8_t lo[kPcfRegs], hi[kPcfRegs];
    pcfAnswered_ = readReg(kPcfControl1, lo, sizeof(lo));
    if (!pcfAnswered_) return false;
    for (std::size_t i = 0; i < sizeof(pcfRegs_); ++i) pcfRegs_[i] = lo[i];
#if HIPI_RTC_CHIP == 3
    return true;
#else
    if (looksLikePcf85063(lo + kPcfSeconds)) return true;
    return readReg(0x20, hi, sizeof(hi)) && pcf85063Fingerprint(lo, hi);
#endif
}

const char* CRtc::chipName() const {
    switch (chip_) {
        case RtcChip::DS3231:    return "DS3231";
        case RtcChip::DS1307:    return "DS1307";
        case RtcChip::PCF85063A: return "PCF85063A";
        default:                 return "RTC";
    }
}

bool CRtc::begin(uint gpioSda, uint gpioScl) {
    initBus(gpioSda, gpioScl);
    present_ = false;
    pcfAnswered_ = false;
    chip_ = RtcChip::None;
#if HIPI_RTC_CHIP != 3
    address_ = kRtcAddrDs;
    std::uint8_t sec = 0;
    if (readReg8(kRegSeconds, sec)) {         // does anything answer at 0x68?
        present_ = true;
        chip_ = detectDsChip();
        return true;
    }
#endif
#if HIPI_RTC_CHIP == 0 || HIPI_RTC_CHIP == 3
    if (findPcf()) {
        present_ = true;
        chip_ = RtcChip::PCF85063A;
        return true;
    }
#endif
    address_ = kRtcAddrDs;
    return false;
}

bool CRtc::valid() {
    if (!present_) return false;
    std::uint8_t v = 0;
    switch (chip_) {
        case RtcChip::DS1307:
            if (!readReg8(kRegSeconds, v)) return false;
            return (v & kClockHalt) == 0;
        case RtcChip::PCF85063A: {
            // Running (no STOP, no EXT_TEST), OS clear, and a real time in
            // the registers (garbled ones may have OS clear too)
            std::uint8_t c1 = 0, r[7];
            if (!readReg8(kPcfControl1, c1) || !readReg(kPcfSeconds, r, sizeof(r))) return false;
            return (c1 & (kPcfStop | kPcfExtTest)) == 0 && (r[0] & kPcfOscStop) == 0 && looksLikePcf85063(r);
        }
        default:
            if (!readReg8(kRegStatus, v)) return false;
            return (v & kOscStopped) == 0;
    }
}

bool CRtc::read(DateTime& t) {
    std::uint8_t r[7];
    if (!present_) return false;
    if (chip_ == RtcChip::PCF85063A) {
        std::uint8_t c1 = 0;
        if (!readReg8(kPcfControl1, c1) || !readReg(kPcfSeconds, r, sizeof(r))) return false;
        t.second = fromBcd(r[0] & 0x7F);     // masks the OS flag
        t.minute = fromBcd(r[1] & 0x7F);
        t.hour = (c1 & kPcf12h) ? hour12(r[2]) : fromBcd(r[2] & 0x3F);   // 12-hour: not set by us
        t.day = fromBcd(r[3] & 0x3F);        // (r[4]: weekday -- computed from the date instead)
        t.month = fromBcd(r[5] & 0x1F);
        t.year = 2000 + fromBcd(r[6]);
        t.weekday = weekdayOf(t.year, t.month, t.day);
        return true;
    }
    if (!readReg(kRegSeconds, r, sizeof(r))) return false;
    t.second = fromBcd(r[0] & 0x7F);         // masks the DS1307's CH bit too
    t.minute = fromBcd(r[1] & 0x7F);
    t.hour = (r[2] & 0x40) ? hour12(r[2])    // 12-hour mode (not set by us)
                           : fromBcd(r[2] & 0x3F);
    t.day = fromBcd(r[4] & 0x3F);
    t.month = fromBcd(r[5] & 0x1F);
    t.year = 2000 + fromBcd(r[6]);
    t.weekday = weekdayOf(t.year, t.month, t.day);
    return true;
}

bool CRtc::set(const DateTime& t) {
    if (!present_) return false;
    if (chip_ == RtcChip::PCF85063A) return hold(t) && release();
    return hold(t);
}

bool CRtc::hold(const DateTime& t) {
    if (!present_) return false;
    const int wd = weekdayOf(t.year, t.month, t.day);
    if (chip_ == RtcChip::PCF85063A) {
        // Garbled registers (no time in them, or EXT_TEST set): start from
        // a software reset (datasheet 8.2.1.3: 0x58 to Control_1; all
        // registers to their reset values, CAP_SEL too). Just stopped or
        // OS set: no reset, the settings stay.
        std::uint8_t now[kPcfSeconds + 7];
        if (!readReg(kPcfControl1, now, sizeof(now))) return false;
        if (((now[0] & kPcfExtTest) != 0 || !looksLikePcf85063(now + kPcfSeconds)) &&
            !writeReg8(kPcfControl1, kPcfSoftReset))
            return false;
        // 24-hour mode before the hours are written (they're read in the
        // mode set), and STOP: the count of the second held at its start
        // until release(); the other bits as they were
        std::uint8_t c1 = 0;
        if (!readReg8(kPcfControl1, c1)) return false;
        if (!writeReg8(kPcfControl1, static_cast<std::uint8_t>((c1 & ~(kPcf12h | kPcfExtTest)) | kPcfStop)))
            return false;
        // Seconds with OS clear: the time is valid from now on
        const std::uint8_t buf[8] = {
            kPcfSeconds, toBcd(t.second), toBcd(t.minute), toBcd(t.hour),
            toBcd(t.day), static_cast<std::uint8_t>(wd),          // weekday 0 = Sunday, as ours
            toBcd(t.month), toBcd(t.year % 100) };
        return i2c_write_blocking(bus_, address_, buf, sizeof(buf), false) == static_cast<int>(sizeof(buf));
    }
    const std::uint8_t buf[8] = {
        kRegSeconds, toBcd(t.second), toBcd(t.minute), toBcd(t.hour),   // 24-hour mode
        static_cast<std::uint8_t>(wd + 1),
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

bool CRtc::release() {
    if (!present_) return false;
    if (chip_ != RtcChip::PCF85063A) return true;            // (it runs already)
    std::uint8_t c1 = 0;
    if (!readReg8(kPcfControl1, c1)) return false;
    return writeReg8(kPcfControl1, static_cast<std::uint8_t>(c1 & ~kPcfStop));
}

}  // namespace hipi
