// bme280.cpp -- see bme280.h.
#include "bme280.h"

namespace hipi {

namespace {

constexpr std::uint8_t kRegCalibTp = 0x88;
constexpr std::uint8_t kRegCalibH1 = 0xA1;
constexpr std::uint8_t kRegId = 0xD0;
constexpr std::uint8_t kRegCalibH = 0xE1;
constexpr std::uint8_t kRegCtrlHum = 0xF2;
constexpr std::uint8_t kRegStatus = 0xF3;        // bit 3: measuring
constexpr std::uint8_t kRegCtrlMeas = 0xF4;
constexpr std::uint8_t kRegData = 0xF7;          // pressure (3 bytes), temperature (3), humidity (2)
constexpr std::uint8_t kOsrsH = 1;               // humidity x1
constexpr std::uint8_t kCtrlMeas = (2 << 5) | (3 << 2) | 1;   // temperature x2, pressure x4, forced mode

std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }   // LSB first
std::int16_t s16(const std::uint8_t* p) { return static_cast<std::int16_t>(u16(p)); }
// A shift to the left of a value that may be negative (a multiplication: shifting one is not defined)
std::int64_t shl(std::int64_t x, int n) { return x * (static_cast<std::int64_t>(1) << n); }

}  // namespace

void bme280_compensate(const std::uint8_t tp[24], std::uint8_t h1, const std::uint8_t h[7], std::int32_t adcT,
                       std::int32_t adcP, std::int32_t adcH, double& tempC, double& pressurePa, double& humidity) {
    const std::int64_t t1 = u16(tp + 0), t2 = s16(tp + 2), t3 = s16(tp + 4);
    const std::int64_t p1 = u16(tp + 6), p2 = s16(tp + 8), p3 = s16(tp + 10), p4 = s16(tp + 12), p5 = s16(tp + 14),
                       p6 = s16(tp + 16), p7 = s16(tp + 18), p8 = s16(tp + 20), p9 = s16(tp + 22);
    const std::int64_t dh1 = h1, dh2 = s16(h + 0), dh3 = h[2];
    // H4 and H5 are 12 bits each, sharing the byte at 0xE5; the sign is the top byte's
    const std::int64_t dh4 = (static_cast<std::int8_t>(h[3]) * 16) | (h[4] & 0x0F);
    const std::int64_t dh5 = (static_cast<std::int8_t>(h[5]) * 16) | (h[4] >> 4);
    const std::int64_t dh6 = static_cast<std::int8_t>(h[6]);

    // Temperature (0.01 deg C); t_fine goes on to the pressure and the humidity
    std::int64_t var1 = (((adcT >> 3) - shl(t1, 1)) * t2) >> 11;
    std::int64_t var2 = (((((adcT >> 4) - t1) * ((adcT >> 4) - t1)) >> 12) * t3) >> 14;
    const std::int64_t tFine = var1 + var2;
    tempC = ((tFine * 5 + 128) >> 8) / 100.0;

    // Pressure (Pa, as Q24.8)
    var1 = tFine - 128000;
    var2 = var1 * var1 * p6;
    var2 = var2 + shl(var1 * p5, 17);
    var2 = var2 + shl(p4, 35);
    var1 = ((var1 * var1 * p3) >> 8) + shl(var1 * p2, 12);
    var1 = ((shl(1, 47) + var1) * p1) >> 33;
    if (var1 == 0) {
        pressurePa = 0;                              // the datasheet's guard against dividing by zero
    } else {
        std::int64_t p = 1048576 - adcP;
        p = ((shl(p, 31) - var2) * 3125) / var1;
        var1 = (p9 * (p >> 13) * (p >> 13)) >> 25;
        var2 = (p8 * p) >> 19;
        p = ((p + var1 + var2) >> 8) + shl(p7, 4);
        pressurePa = p / 256.0;
    }

    // Humidity (%RH, as Q22.10)
    std::int64_t v = tFine - 76800;
    v = ((((shl(adcH, 14) - shl(dh4, 20) - (dh5 * v)) + 16384) >> 15) *
         (((((((v * dh6) >> 10) * (((v * dh3) >> 11) + 32768)) >> 10) + 2097152) * dh2 + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * dh1) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    humidity = (v >> 12) / 1024.0;
}

bool bme280_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa, double& humidity) {
    std::uint8_t id = 0;
    if (!bus.readRegs(addr, kRegId, &id, 1) || id != 0x60) return false;
    std::uint8_t tp[24], h1 = 0, h[7];
    if (!bus.readRegs(addr, kRegCalibTp, tp, sizeof(tp)) || !bus.readRegs(addr, kRegCalibH1, &h1, 1) ||
        !bus.readRegs(addr, kRegCalibH, h, sizeof(h)))
        return false;
    // ctrl_hum counts only after ctrl_meas has been written (datasheet 5.4.3)
    const std::uint8_t hum[2] = { kRegCtrlHum, kOsrsH };
    const std::uint8_t meas[2] = { kRegCtrlMeas, kCtrlMeas };
    if (!bus.write(addr, hum, 2) || !bus.write(addr, meas, 2)) return false;
    // At most 1.25 + 2.3*2 + (2.3*4 + 0.575) + (2.3*1 + 0.575) = 18.5 ms (datasheet 9.1)
    bus.delayMs(20);
    std::uint8_t st = 0x08;
    for (int waited = 20; waited <= 100 && (st & 0x08); waited += 5) {
        if (!bus.readRegs(addr, kRegStatus, &st, 1)) return false;
        if (st & 0x08) bus.delayMs(5);
    }
    if (st & 0x08) return false;
    std::uint8_t d[8];
    if (!bus.readRegs(addr, kRegData, d, sizeof(d))) return false;   // one burst: never two measurements mixed
    const std::int32_t adcP = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
    const std::int32_t adcT = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
    const std::int32_t adcH = (d[6] << 8) | d[7];
    bme280_compensate(tp, h1, h, adcT, adcP, adcH, tempC, pressurePa, humidity);
    return true;
}

}  // namespace hipi
