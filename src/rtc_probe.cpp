// rtc_probe.cpp -- see rtc_probe.h
#include "rtc_probe.h"
#include "rtc.h"              // looksLikePcf85063()
#include "pico/time.h"
#include <cstdio>

namespace hipi {

namespace {

int bcd(std::uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
bool isBcd(std::uint8_t v, int max) { return (v & 0x0F) <= 9 && bcd(v) <= max; }

// A clock chip's seconds should move between two looks (at least 1.5 s
// apart): true if they didn't. `s` = seconds + 60 * minutes; kept per address.
bool clockStopped(std::uint8_t addr, int s) {
    static int lastSec[128];
    static std::uint64_t lastUs[128];
    static bool seen[128];
    const std::uint64_t now = time_us_64();
    addr &= 0x7F;
    bool stopped = false;
    if (!seen[addr]) {
        seen[addr] = true;
        lastSec[addr] = s;
        lastUs[addr] = now;
    } else if (now - lastUs[addr] >= 1500000) {
        stopped = s == lastSec[addr];
        lastSec[addr] = s;
        lastUs[addr] = now;
    }
    return stopped;
}

// "12:34:56" + extra + "  stopped" / "  time lost" -- and the verdict
I2cVerdict clockVerdict(std::uint8_t addr, int hr, int min, int sec, bool lost, const char* extra,
                        std::string& detail) {
    const bool stopped = clockStopped(addr, sec + 60 * min);
    char b[48];
    int n = std::snprintf(b, sizeof(b), "%02d:%02d:%02d%s", hr, min, sec, extra);
    if (n < 0 || n >= static_cast<int>(sizeof(b))) n = static_cast<int>(sizeof(b)) - 1;
    if (stopped) std::snprintf(b + n, sizeof(b) - static_cast<std::size_t>(n), "  stopped");
    else if (lost) std::snprintf(b + n, sizeof(b) - static_cast<std::size_t>(n), "  time lost");
    detail = b;
    return stopped || lost ? I2cVerdict::Fault : I2cVerdict::Ok;
}

}  // namespace

I2cVerdict rtc_probeDs(I2cBus& bus, std::uint8_t addr, std::string& name, std::string& detail) {
    std::uint8_t t[0x13];
    if (!bus.readRegs(addr, 0x00, t, sizeof(t))) return I2cVerdict::NotIt;
    const std::uint8_t sec = t[0] & 0x7F, min = t[1] & 0x7F, hr = t[2] & 0x3F;
    if (!isBcd(sec, 59) || !isBcd(min, 59) || !isBcd(hr, 23) ||
        !isBcd(t[4] & 0x3F, 31) || !isBcd(t[5] & 0x1F, 12)) return I2cVerdict::NotIt;
    const int tempQ = static_cast<std::int8_t>(t[0x11]) * 4 + (t[0x12] >> 6);
    const bool ds3231 = (t[0x12] & 0x3F) == 0 && tempQ > -40 * 4 && tempQ < 85 * 4;
    name = ds3231 ? "DS3231" : "DS1307";
    const bool lost = ds3231 ? (t[0x0F] & 0x80) != 0 : (t[0] & 0x80) != 0;   // OSF / clock halt
    char temp[20] = "";
    if (ds3231)
        std::snprintf(temp, sizeof(temp), "  %d.%02d C", tempQ / 4, (tempQ < 0 ? -tempQ : tempQ) % 4 * 25);
    return clockVerdict(addr, bcd(hr), bcd(min), bcd(sec), lost, temp, detail);
}

I2cVerdict rtc_probePcf85063(I2cBus& bus, std::uint8_t addr, std::string& name, std::string& detail) {
    std::uint8_t t[kPcfRegs];                            // Control_1 .. Timer_mode
    if (!bus.readRegs(addr, 0x00, t, sizeof(t))) return I2cVerdict::NotIt;
    if (!looksLikePcf85063(t + 4)) {
        // No time in it -- garbled registers (power-up, plugged in while
        // powered)? Then its fingerprint tells it (rtc.h)
        std::uint8_t hi[kPcfRegs];
        if (!bus.readRegs(addr, 0x20, hi, sizeof(hi)) || !pcf85063Fingerprint(t, hi)) return I2cVerdict::NotIt;
        name = "PCF85063A";
        detail = "registers garbled -- set the time";
        return I2cVerdict::Fault;
    }
    name = "PCF85063A";
    const bool h12 = (t[0] & 0x02) != 0;                 // Control_1: 12_24
    const int hr = h12 ? bcd(t[6] & 0x1F) % 12 + ((t[6] & 0x20) ? 12 : 0) : bcd(t[6] & 0x3F);
    const bool lost = (t[4] & 0x80) != 0 || (t[0] & 0xA0) != 0;   // OS / STOP, EXT_TEST
    return clockVerdict(addr, hr, bcd(t[5] & 0x7F), bcd(t[4] & 0x7F), lost, "", detail);
}

}  // namespace hipi
