#ifndef __BMP280_H__
#define __BMP280_H__

#include "i2c_device.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CBmp280 -- Bosch BMP280 digital pressure/temperature sensor
//
//  Register map, calibration layout, and both integer compensation
//  formulas below are taken directly from Bosch's own BMP280 datasheet
//  (rev 1.26, Oct 2021), section 3.11.3 / 4.2.3 -- the exact
//  bmp280_compensate_T_int32() and bmp280_compensate_P_int64() reference
//  implementations, not a reimplementation from scratch, since even small
//  deviations there translate directly into wrong readings. Both verified
//  separately against a known-good (adc_T, adc_P, dig_T1..3, dig_P1..9)
//  -> (temperature, pressure) test vector before being used here, rather
//  than trusting the transcription alone.
//
//  Pressure compensation depends on t_fine, an intermediate result the
//  temperature compensation computes as a side effect (see the
//  datasheet's own note on this) -- readPressurePa() below always
//  compensates temperature first internally for exactly this reason,
//  even though it only returns the pressure result.
// ─────────────────────────────────────────────────────────────────────────────
class CBmp280 : public CI2CDevice {
public:
    // osrs_t/osrs_p field values (ctrl_meas register, bits [7:5]/[4:2]) --
    // higher = more oversampling = less noise, longer conversion time.
    // Applied to BOTH temperature and pressure together (see
    // setOversampling()'s own comment) -- Skip (000) disables that
    // measurement entirely; not offered here since "no reading at all"
    // isn't a meaningful precision level for what this class is for.
    enum class Oversampling : std::uint8_t {
        X1  = 1,
        X2  = 2,
        X4  = 3,
        X8  = 4,
        X16 = 5,
    };

    // address: 0x76 (SDO tied low) or 0x77 (SDO tied high/floating,
    // BMP280's own default) -- see the datasheet's own section 5.2.
    explicit CBmp280(i2c_inst_t* bus, std::uint8_t address = 0x76)
        : CI2CDevice(bus, address) {}

    // Brings up the shared bus (see CI2CDevice::initBus()'s own comment
    // -- normally a no-op, touchInit() already did this), confirms the
    // chip responds with the expected BMP280 chip-ID (0x58), and reads
    // its factory calibration data. Returns false (device unusable) if
    // either check fails -- e.g. nothing's actually wired up at this
    // address.
    bool begin(uint gpioSda, uint gpioScl);

    // A single oversampling setting applied to both temperature AND
    // pressure -- the chip measures both together in one forced-mode
    // conversion regardless (see readRaw()'s own comment), so there's no
    // real cost to keeping this as one simple knob rather than two
    // independent ones, matching this project's own "enkla settings"
    // (simple settings) brief.
    void setOversampling(Oversampling osrs) { oversampling_ = osrs; }
    Oversampling oversampling() const { return oversampling_; }

    // Triggers a fresh forced-mode conversion at the current
    // oversampling setting, waits for it to complete, and returns
    // temperature in hundredths of a degree C (e.g. 2358 means 23.58
    // degC) via centiDegC. Returns false (centiDegC left unchanged) if
    // the chip never finished converting within a generous timeout --
    // e.g. not actually present.
    bool readTemperatureCenti(std::int32_t& centiDegC);

    // Triggers a fresh forced-mode conversion (temperature is measured
    // and compensated internally too, even though only pressure comes
    // back -- see this class's own header comment for why) and returns
    // pressure in whole Pascals (e.g. 101325 for standard atmospheric
    // pressure) via pascals. Returns false (pascals left unchanged) on
    // the same conditions as readTemperatureCenti().
    bool readPressurePa(std::int32_t& pascals);

private:
    static constexpr std::uint8_t kRegCalibStart = 0x88;
    static constexpr std::uint8_t kRegChipId     = 0xD0;
    static constexpr std::uint8_t kRegStatus     = 0xF3;
    static constexpr std::uint8_t kRegCtrlMeas   = 0xF4;
    static constexpr std::uint8_t kRegPressMsb   = 0xF7;  // press MSB..XLSB (0xF7-F9)
                                                             // then temp MSB..XLSB (0xFA-FC)
                                                             // immediately follow -- one burst
                                                             // covers both, see readRaw()
    static constexpr std::uint8_t kChipId        = 0x58;

    // Shared by readTemperatureCenti()/readPressurePa(): writes
    // ctrl_meas to start a forced-mode conversion (the chip measures
    // temperature AND pressure together in one cycle whenever both
    // osrs_t/osrs_p are non-zero, which they always are here -- see
    // setOversampling()'s own comment), polls the status register until
    // the "measuring" bit clears, then reads back both raw 20-bit ADC
    // values. Returns false or false (adc_T/adc_P left unchanged) if the
    // chip never finished converting within a generous timeout, or a
    // register access itself failed.
    bool readRaw(std::int32_t& adc_T, std::int32_t& adc_P);

    // Bosch's own reference compensation formulas (see this file's own
    // header comment). compensateTemperature() also sets t_fine_, which
    // compensatePressurePa() needs -- always call compensateTemperature()
    // first for a given reading, exactly as the datasheet itself
    // requires.
    std::int32_t compensateTemperature(std::int32_t adc_T);
    std::int32_t compensatePressurePa(std::int32_t adc_P);

    Oversampling oversampling_ = Oversampling::X4;

    // Factory calibration words -- see the datasheet's own Table 17 for
    // this exact layout/naming.
    std::uint16_t dig_T1_ = 0;
    std::int16_t  dig_T2_ = 0, dig_T3_ = 0;
    std::uint16_t dig_P1_ = 0;
    std::int16_t  dig_P2_ = 0, dig_P3_ = 0, dig_P4_ = 0, dig_P5_ = 0,
                  dig_P6_ = 0, dig_P7_ = 0, dig_P8_ = 0, dig_P9_ = 0;

    std::int64_t t_fine_ = 0;
};

#endif//__BMP280_H__
