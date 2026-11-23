// bme680.cpp -- see bme680.h.
#include "bme680.h"

namespace hipi {

namespace {

constexpr std::uint8_t kRegCalibA = 0x8A;        // 23 bytes
constexpr std::uint8_t kRegCalibB = 0xE1;        // 10 bytes
constexpr std::uint8_t kRegId = 0xD0;
constexpr std::uint8_t kRegCtrlGas1 = 0x71;      // bit 4: run_gas
constexpr std::uint8_t kRegCtrlHum = 0x72;
constexpr std::uint8_t kRegCtrlMeas = 0x74;
constexpr std::uint8_t kRegData = 0x1D;          // status, 1 spare, pressure (3), temperature (3), humidity (2)
constexpr std::uint8_t kOsrsH = 1;               // humidity x1
constexpr std::uint8_t kCtrlMeas = (2 << 5) | (3 << 2) | 1;   // temperature x2, pressure x4, forced mode

std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }   // LSB first
std::int16_t s16(const std::uint8_t* p) { return static_cast<std::int16_t>(u16(p)); }
std::int8_t s8(const std::uint8_t* p) { return static_cast<std::int8_t>(*p); }

}  // namespace

void bme680_compensate(const std::uint8_t a[23], const std::uint8_t b[10], std::int32_t adcT, std::int32_t adcP,
                       std::int32_t adcH, double& tempC, double& pressurePa, double& humidity) {
    // The calibration (the data types are those of Bosch's own API)
    const double t1 = u16(b + 8), t2 = s16(a + 0), t3 = s8(a + 2);
    const double p1 = u16(a + 4), p2 = s16(a + 6), p3 = s8(a + 8), p4 = s16(a + 10), p5 = s16(a + 12),
                 p6 = s8(a + 15), p7 = s8(a + 14), p8 = s16(a + 18), p9 = s16(a + 20), p10 = a[22];
    const double h1 = (b[2] << 4) | (b[1] & 0x0F);                // 12 bits: 0xE3 and the low nibble of 0xE2
    const double h2 = (b[0] << 4) | (b[1] >> 4);                  // 12 bits: 0xE1 and the high nibble of 0xE2
    const double h3 = s8(b + 3), h4 = s8(b + 4), h5 = s8(b + 5), h6 = b[6], h7 = s8(b + 7);

    // Temperature (datasheet 3.3.1)
    double var1 = ((adcT / 16384.0) - (t1 / 1024.0)) * t2;
    double var2 = (((adcT / 131072.0) - (t1 / 8192.0)) * ((adcT / 131072.0) - (t1 / 8192.0))) * (t3 * 16.0);
    const double tFine = var1 + var2;
    const double temp = tFine / 5120.0;
    tempC = temp;

    // Pressure (3.3.2)
    var1 = (tFine / 2.0) - 64000.0;
    var2 = var1 * var1 * (p6 / 131072.0);
    var2 = var2 + (var1 * p5 * 2.0);
    var2 = (var2 / 4.0) + (p4 * 65536.0);
    var1 = (((p3 * var1 * var1) / 16384.0) + (p2 * var1)) / 524288.0;
    var1 = (1.0 + (var1 / 32768.0)) * p1;
    if (var1 == 0.0) {
        pressurePa = 0;                                           // not a sensible calibration
    } else {
        double p = 1048576.0 - adcP;
        p = ((p - (var2 / 4096.0)) * 6250.0) / var1;
        var1 = (p9 * p * p) / 2147483648.0;
        var2 = p * (p8 / 32768.0);
        const double var3 = (p / 256.0) * (p / 256.0) * (p / 256.0) * (p10 / 131072.0);
        pressurePa = p + (var1 + var2 + var3 + (p7 * 128.0)) / 16.0;
    }

    // Humidity (3.3.3)
    var1 = adcH - ((h1 * 16.0) + ((h3 / 2.0) * temp));
    var2 = var1 * ((h2 / 262144.0) * (1.0 + ((h4 / 16384.0) * temp) + ((h5 / 1048576.0) * temp * temp)));
    const double var3 = h6 / 16384.0;
    const double var4 = h7 / 2097152.0;
    double h = var2 + ((var3 + (var4 * temp)) * var2 * var2);
    if (h < 0.0) h = 0.0;
    if (h > 100.0) h = 100.0;
    humidity = h;
}

bool bme680_read(I2cBus& bus, std::uint8_t addr, double& tempC, double& pressurePa, double& humidity) {
    std::uint8_t id = 0;
    if (!bus.readRegs(addr, kRegId, &id, 1) || id != 0x61) return false;
    std::uint8_t a[23], b[10];
    if (!bus.readRegs(addr, kRegCalibA, a, sizeof(a)) || !bus.readRegs(addr, kRegCalibB, b, sizeof(b))) return false;
    // No gas measurement; the humidity setting counts when ctrl_meas is written
    const std::uint8_t gas[2] = { kRegCtrlGas1, 0x00 };
    const std::uint8_t hum[2] = { kRegCtrlHum, kOsrsH };
    const std::uint8_t meas[2] = { kRegCtrlMeas, kCtrlMeas };
    if (!bus.write(addr, gas, 2) || !bus.write(addr, hum, 2) || !bus.write(addr, meas, 2)) return false;
    // About 30 ms (the sequence also holds a gas slot, a dummy one): wait, then wait for "new data" (up to 200 ms in all)
    bus.delayMs(30);
    std::uint8_t d[10];
    bool ready = false;
    for (int waited = 30; waited <= 200; waited += 10) {
        if (!bus.readRegs(addr, kRegData, d, 1)) return false;
        if ((d[0] & 0x80) != 0 && (d[0] & 0x20) == 0) { ready = true; break; }   // new data, and not measuring
        bus.delayMs(10);
    }
    if (!ready) return false;
    if (!bus.readRegs(addr, kRegData, d, sizeof(d))) return false;     // one burst: never two measurements mixed
    const std::int32_t adcP = (d[2] << 12) | (d[3] << 4) | (d[4] >> 4);
    const std::int32_t adcT = (d[5] << 12) | (d[6] << 4) | (d[7] >> 4);
    const std::int32_t adcH = (d[8] << 8) | d[9];
    bme680_compensate(a, b, adcT, adcP, adcH, tempC, pressurePa, humidity);
    return true;
}

}  // namespace hipi
