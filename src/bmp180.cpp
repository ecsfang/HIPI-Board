// bmp180.cpp -- see bmp180.h.
#include "bmp180.h"

namespace hipi {

namespace {

constexpr std::uint8_t kRegId = 0xD0;
constexpr std::uint8_t kRegCalib = 0xAA;
constexpr std::uint8_t kRegCtrl = 0xF4;
constexpr std::uint8_t kRegOut = 0xF6;           // MSB, LSB, (XLSB)
constexpr std::uint8_t kCmdTemp = 0x2E;          // 4.5 ms
constexpr std::uint8_t kCmdPress = 0x34;         // | oss << 6; 25.5 ms at oss 3
constexpr int kOss = 3;                          // ultra high resolution

std::int16_t s16(const std::uint8_t* p) { return static_cast<std::int16_t>((p[0] << 8) | p[1]); }   // MSB first
std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

}  // namespace

void bmp180_compensate(const std::uint8_t c[22], std::int32_t ut, std::int32_t up, int oss, std::int32_t& temp01C,
                       std::int32_t& pressurePa) {
    const std::int64_t ac1 = s16(c + 0), ac2 = s16(c + 2), ac3 = s16(c + 4);
    const std::int64_t ac4 = u16(c + 6), ac5 = u16(c + 8), ac6 = u16(c + 10);
    const std::int64_t b1 = s16(c + 12), b2 = s16(c + 14);
    const std::int64_t mc = s16(c + 18), md = s16(c + 20);       // (MB, at c + 16, is not used)

    // True temperature
    std::int64_t x1 = ((ut - ac6) * ac5) >> 15;
    std::int64_t x2 = (mc * 2048) / (x1 + md);
    const std::int64_t b5 = x1 + x2;
    temp01C = static_cast<std::int32_t>((b5 + 8) >> 4);

    // True pressure
    const std::int64_t b6 = b5 - 4000;
    x1 = (b2 * ((b6 * b6) >> 12)) >> 11;
    x2 = (ac2 * b6) >> 11;
    std::int64_t x3 = x1 + x2;
    const std::int64_t b3 = ((((ac1 * 4 + x3) * (1 << oss)) + 2)) >> 2;     // (a shift of a negative value is not defined)
    x1 = (ac3 * b6) >> 13;
    x2 = (b1 * ((b6 * b6) >> 12)) >> 16;
    x3 = ((x1 + x2) + 2) >> 2;
    const std::int64_t b4 = (ac4 * (x3 + 32768)) >> 15;
    const std::uint32_t b7 = static_cast<std::uint32_t>((up - b3) * (50000 >> oss));
    std::int64_t p = b7 < 0x80000000u ? (static_cast<std::int64_t>(b7) * 2) / b4 : (static_cast<std::int64_t>(b7) / b4) * 2;
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    p = p + ((x1 + x2 + 3791) >> 4);
    pressurePa = static_cast<std::int32_t>(p);
}

bool bmp180_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa) {
    std::uint8_t id = 0;
    if (!bus.readRegs(addr, kRegId, &id, 1) || id != 0x55) return false;
    std::uint8_t calib[22];
    if (!bus.readRegs(addr, kRegCalib, calib, sizeof(calib))) return false;
    for (int i = 0; i < 22; i += 2) {                            // the datasheet's check that the reading worked
        const std::uint16_t w = u16(calib + i);
        if (w == 0 || w == 0xFFFF) return false;
    }

    const std::uint8_t tempCmd[2] = { kRegCtrl, kCmdTemp };
    if (!bus.write(addr, tempCmd, 2)) return false;
    bus.delayMs(5);
    std::uint8_t d[3];
    if (!bus.readRegs(addr, kRegOut, d, 2)) return false;
    const std::int32_t ut = (d[0] << 8) | d[1];

    const std::uint8_t pressCmd[2] = { kRegCtrl, static_cast<std::uint8_t>(kCmdPress | (kOss << 6)) };
    if (!bus.write(addr, pressCmd, 2)) return false;
    bus.delayMs(26);
    if (!bus.readRegs(addr, kRegOut, d, 3)) return false;
    const std::int32_t up = ((d[0] << 16) | (d[1] << 8) | d[2]) >> (8 - kOss);

    std::int32_t t01 = 0, pa = 0;
    bmp180_compensate(calib, ut, up, kOss, t01, pa);
    tempC = t01 / 10.0;
    pressurePa = pa;
    return true;
}

}  // namespace hipi
