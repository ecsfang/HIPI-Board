// bmp390.cpp -- see bmp390.h.
#include "bmp390.h"
#include <cmath>

namespace hipi {

namespace {

constexpr std::uint8_t kRegId = 0x00;
constexpr std::uint8_t kRegStatus = 0x03;        // bit 5: pressure ready, bit 6: temperature ready
constexpr std::uint8_t kRegData = 0x04;          // pressure (3 bytes), then temperature (3 bytes)
constexpr std::uint8_t kRegPwrCtrl = 0x1B;
constexpr std::uint8_t kRegOsr = 0x1C;
constexpr std::uint8_t kRegCalib = 0x31;
constexpr std::uint8_t kOsr = 0x03;              // osr_p = x8 (bits 2..0), osr_t = x1 (bits 5..3)
constexpr std::uint8_t kForcedBoth = 0x13;       // mode forced (bits 5..4 = 01), temperature and pressure on

std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::int16_t s16(const std::uint8_t* p) { return static_cast<std::int16_t>(u16(p)); }
std::int8_t s8(const std::uint8_t* p) { return static_cast<std::int8_t>(*p); }

}  // namespace

void bmp390_compensate(const std::uint8_t c[21], std::uint32_t rawT, std::uint32_t rawP, double& tempC,
                       double& pressurePa) {
    // The coefficients (datasheet table 24 and section 8.4: NVM value / 2^n)
    const double t1 = u16(c + 0) * 256.0;                         // / 2^-8
    const double t2 = u16(c + 2) / 1073741824.0;                  // / 2^30
    const double t3 = s8(c + 4) / 281474976710656.0;              // / 2^48
    const double p1 = (s16(c + 5) - 16384.0) / 1048576.0;         // (x - 2^14) / 2^20
    const double p2 = (s16(c + 7) - 16384.0) / 536870912.0;       // (x - 2^14) / 2^29
    const double p3 = s8(c + 9) / 4294967296.0;                   // / 2^32
    const double p4 = s8(c + 10) / 137438953472.0;                // / 2^37
    const double p5 = u16(c + 11) * 8.0;                          // / 2^-3
    const double p6 = u16(c + 13) / 64.0;                         // / 2^6
    const double p7 = s8(c + 15) / 256.0;                         // / 2^8
    const double p8 = s8(c + 16) / 32768.0;                       // / 2^15
    const double p9 = s16(c + 17) / 281474976710656.0;            // / 2^48
    const double p10 = s8(c + 19) / 281474976710656.0;            // / 2^48
    const double p11 = s8(c + 20) / 36893488147419103232.0;       // / 2^65

    // Temperature (section 8.5); t is also what the pressure needs
    const double d1 = static_cast<double>(rawT) - t1;
    const double t = d1 * t2 + d1 * d1 * t3;
    tempC = t;

    // Pressure (section 8.6)
    const double up = static_cast<double>(rawP);
    const double out1 = p5 + p6 * t + p7 * t * t + p8 * t * t * t;
    const double out2 = up * (p1 + p2 * t + p3 * t * t + p4 * t * t * t);
    const double out3 = up * up * (p9 + p10 * t) + up * up * up * p11;
    pressurePa = out1 + out2 + out3;
}

bool bmp390_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa) {
    std::uint8_t id = 0;
    if (!bus.readRegs(addr, kRegId, &id, 1) || (id != 0x60 && id != 0x50)) return false;
    std::uint8_t calib[21];
    if (!bus.readRegs(addr, kRegCalib, calib, sizeof(calib))) return false;
    const std::uint8_t osr[2] = { kRegOsr, kOsr };
    const std::uint8_t go[2] = { kRegPwrCtrl, kForcedBoth };
    if (!bus.write(addr, osr, 2) || !bus.write(addr, go, 2)) return false;
    // Typically 18.7 ms, at most 21.5 ms (datasheet table 23): wait, then
    // see that both values are ready (up to 100 ms in all)
    bus.delayMs(22);
    std::uint8_t st = 0;
    for (int waited = 22; waited <= 100; waited += 6) {
        if (bus.readRegs(addr, kRegStatus, &st, 1) && (st & 0x60) == 0x60) break;
        bus.delayMs(6);
    }
    if ((st & 0x60) != 0x60) return false;
    std::uint8_t d[6];
    if (!bus.readRegs(addr, kRegData, d, sizeof(d))) return false;   // one burst: never two measurements mixed
    const std::uint32_t rawP = d[0] | (d[1] << 8) | (static_cast<std::uint32_t>(d[2]) << 16);
    const std::uint32_t rawT = d[3] | (d[4] << 8) | (static_cast<std::uint32_t>(d[5]) << 16);
    bmp390_compensate(calib, rawT, rawP, tempC, pressurePa);
    return std::isfinite(tempC) && std::isfinite(pressurePa);
}

}  // namespace hipi
