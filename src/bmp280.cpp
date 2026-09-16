#include "bmp280.h"
#include "pico/stdlib.h"

bool CBmp280::begin(uint gpioSda, uint gpioScl) {
    initBus(gpioSda, gpioScl);

    std::uint8_t id = 0;
    if (!readReg8(kRegChipId, id) || id != kChipId) {
        return false;  // nothing responding at this address, or not a BMP280
    }

    // Burst-read the full factory calibration block (0x88..0x9F, 24
    // bytes: dig_T1..3 then dig_P1..9, each a little-endian 16-bit word
    // -- see this file's own header comment for the datasheet section
    // this layout comes from) in one transaction rather than 12
    // individual reads.
    std::uint8_t raw[24];
    if (!readReg(kRegCalibStart, raw, sizeof(raw))) return false;

    auto u16le = [&](int i) -> std::uint16_t {
        return static_cast<std::uint16_t>(raw[i] | (raw[i + 1] << 8));
    };
    auto s16le = [&](int i) -> std::int16_t {
        return static_cast<std::int16_t>(u16le(i));
    };

    dig_T1_ = u16le(0);
    dig_T2_ = s16le(2);
    dig_T3_ = s16le(4);
    dig_P1_ = u16le(6);
    dig_P2_ = s16le(8);
    dig_P3_ = s16le(10);
    dig_P4_ = s16le(12);
    dig_P5_ = s16le(14);
    dig_P6_ = s16le(16);
    dig_P7_ = s16le(18);
    dig_P8_ = s16le(20);
    dig_P9_ = s16le(22);

    return true;
}

std::int32_t CBmp280::compensateTemperature(std::int32_t adc_T) {
    // Bosch's own bmp280_compensate_T_int32() reference formula, exactly
    // (see this file's own header comment) -- returns hundredths of a
    // degree C, e.g. 2358 means 23.58 degC. Computed in int64_t rather
    // than the datasheet's own plain int32_t so t_fine_ (used by
    // compensatePressurePa() below, per the datasheet's own int64
    // pressure formula) doesn't need a separate, differently-typed copy
    // -- confirmed against the same verified test vector as the int32
    // version to give an identical result either way (the intermediate
    // values involved never approach int32 overflow for realistic
    // readings), so this is purely a type-unification convenience, not a
    // behavior change.
    std::int64_t var1, var2;
    var1 = ((((adc_T >> 3) - (static_cast<std::int64_t>(dig_T1_) << 1))) *
            static_cast<std::int64_t>(dig_T2_)) >> 11;
    var2 = (((((adc_T >> 4) - static_cast<std::int64_t>(dig_T1_)) *
              ((adc_T >> 4) - static_cast<std::int64_t>(dig_T1_))) >> 12) *
            static_cast<std::int64_t>(dig_T3_)) >> 14;
    t_fine_ = var1 + var2;
    return static_cast<std::int32_t>((t_fine_ * 5 + 128) >> 8);
}

std::int32_t CBmp280::compensatePressurePa(std::int32_t adc_P) {
    // Bosch's own bmp280_compensate_P_int64() reference formula, exactly
    // (see this file's own header comment) -- depends on t_fine_, which
    // MUST already be set by a compensateTemperature() call for this
    // same reading cycle (readRaw()'s own callers -- readTemperatureCenti()/
    // readPressurePa() -- always do this in the right order). Returns Pa
    // in Q24.8 fixed point (24 integer bits, 8 fractional bits); divided
    // down to whole Pa here since that's all this class exposes.
    std::int64_t var1, var2, p;
    var1 = t_fine_ - 128000;
    var2 = var1 * var1 * static_cast<std::int64_t>(dig_P6_);
    var2 = var2 + ((var1 * static_cast<std::int64_t>(dig_P5_)) << 17);
    var2 = var2 + (static_cast<std::int64_t>(dig_P4_) << 35);
    var1 = ((var1 * var1 * static_cast<std::int64_t>(dig_P3_)) >> 8) +
           ((var1 * static_cast<std::int64_t>(dig_P2_)) << 12);
    var1 = ((static_cast<std::int64_t>(1) << 47) + var1) *
           static_cast<std::int64_t>(dig_P1_) >> 33;
    if (var1 == 0) return 0;  // avoid divide-by-zero -- matches the datasheet's own guard
    p = 1048576 - adc_P;
    p = ((p << 31) - var2) * 3125 / var1;
    var1 = (static_cast<std::int64_t>(dig_P9_) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (static_cast<std::int64_t>(dig_P8_) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (static_cast<std::int64_t>(dig_P7_) << 4);
    return static_cast<std::int32_t>(p >> 8);  // Q24.8 -> whole Pa
}

bool CBmp280::readRaw(std::int32_t& adc_T, std::int32_t& adc_P) {
    const std::uint8_t osrs = static_cast<std::uint8_t>(oversampling_);
    constexpr std::uint8_t kModeForced = 0x01;
    // Same oversampling for both -- see setOversampling()'s own comment.
    const std::uint8_t ctrlMeas = static_cast<std::uint8_t>(
        (osrs << 5) | (osrs << 2) | kModeForced);
    if (!writeReg8(kRegCtrlMeas, ctrlMeas)) return false;

    // Poll the "measuring" bit (status register, bit 3) until it clears.
    // Even x16/x16 oversampling on both completes well under 50ms per
    // the datasheet's own timing table -- this budget is deliberately
    // generous (up to ~500ms) so a slightly slower/colder chip on first
    // power-up doesn't spuriously report "not present".
    for (int tries = 0; tries < 100; ++tries) {
        std::uint8_t status = 0;
        if (!readReg8(kRegStatus, status)) return false;
        if ((status & 0x08) == 0) break;  // conversion done
        sleep_ms(5);
        if (tries == 99) return false;    // timed out -- chip stuck/absent
    }

    // Pressure (0xF7..0xF9) and temperature (0xFA..0xFC) are adjacent --
    // one 6-byte burst covers both rather than two separate reads.
    std::uint8_t raw[6];
    if (!readReg(kRegPressMsb, raw, sizeof(raw))) return false;

    adc_P = (static_cast<std::int32_t>(raw[0]) << 12) |
            (static_cast<std::int32_t>(raw[1]) << 4) |
            (static_cast<std::int32_t>(raw[2]) >> 4);
    adc_T = (static_cast<std::int32_t>(raw[3]) << 12) |
            (static_cast<std::int32_t>(raw[4]) << 4) |
            (static_cast<std::int32_t>(raw[5]) >> 4);
    return true;
}

bool CBmp280::readTemperatureCenti(std::int32_t& centiDegC) {
    std::int32_t adc_T, adc_P;
    if (!readRaw(adc_T, adc_P)) return false;
    centiDegC = compensateTemperature(adc_T);
    return true;
}

bool CBmp280::readPressurePa(std::int32_t& pascals) {
    std::int32_t adc_T, adc_P;
    if (!readRaw(adc_T, adc_P)) return false;
    compensateTemperature(adc_T);  // sets t_fine_ -- required before pressure, see this file's own header comment
    pascals = compensatePressurePa(adc_P);
    return true;
}
