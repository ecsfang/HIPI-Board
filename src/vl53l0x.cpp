// vl53l0x.cpp -- see vl53l0x.h
//
// The set-up sequence, the tuning table and the timing arithmetic follow
// ST's VL53L0X API (STSW-IMG005), as condensed in Pololu's VL53L0X library.
#define MODULE "VL53L0X"
#include "vl53l0x.h"
#include "usb_serial.h"
#include <map>

namespace hipi {

namespace {

// Registers
enum : std::uint8_t {
    SYSRANGE_START = 0x00,
    SYSTEM_SEQUENCE_CONFIG = 0x01,
    SYSTEM_INTERRUPT_CONFIG_GPIO = 0x0A,
    SYSTEM_INTERRUPT_CLEAR = 0x0B,
    RESULT_INTERRUPT_STATUS = 0x13,
    RESULT_RANGE_STATUS = 0x14,
    FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT = 0x44,
    MSRC_CONFIG_TIMEOUT_MACROP = 0x46,
    DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD = 0x4E,
    DYNAMIC_SPAD_REF_EN_START_OFFSET = 0x4F,
    PRE_RANGE_CONFIG_VCSEL_PERIOD = 0x50,
    PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI = 0x51,
    MSRC_CONFIG_CONTROL = 0x60,
    FINAL_RANGE_CONFIG_VCSEL_PERIOD = 0x70,
    FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI = 0x71,
    GPIO_HV_MUX_ACTIVE_HIGH = 0x84,
    I2C_MODE = 0x88,
    VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV = 0x89,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_0 = 0xB0,
    GLOBAL_CONFIG_REF_EN_START_SELECT = 0xB6,
};

// What the set-up leaves in SYSTEM_SEQUENCE_CONFIG: still there = still set up
constexpr std::uint8_t kSequence = 0xE8;
constexpr int kWaitMs = 100;          // at most, for anything the chip does

// ST's default tuning settings (register, value), written as they come
constexpr std::uint8_t kTuning[][2] = {
    { 0xFF, 0x01 }, { 0x00, 0x00 }, { 0xFF, 0x00 }, { 0x09, 0x00 }, { 0x10, 0x00 }, { 0x11, 0x00 },
    { 0x24, 0x01 }, { 0x25, 0xFF }, { 0x75, 0x00 }, { 0xFF, 0x01 }, { 0x4E, 0x2C }, { 0x48, 0x00 },
    { 0x30, 0x20 }, { 0xFF, 0x00 }, { 0x30, 0x09 }, { 0x54, 0x00 }, { 0x31, 0x04 }, { 0x32, 0x03 },
    { 0x40, 0x83 }, { 0x46, 0x25 }, { 0x60, 0x00 }, { 0x27, 0x00 }, { 0x50, 0x06 }, { 0x51, 0x00 },
    { 0x52, 0x96 }, { 0x56, 0x08 }, { 0x57, 0x30 }, { 0x61, 0x00 }, { 0x62, 0x00 }, { 0x64, 0x00 },
    { 0x65, 0x00 }, { 0x66, 0xA0 }, { 0xFF, 0x01 }, { 0x22, 0x32 }, { 0x47, 0x14 }, { 0x49, 0xFF },
    { 0x4A, 0x00 }, { 0xFF, 0x00 }, { 0x7A, 0x0A }, { 0x7B, 0x00 }, { 0x78, 0x21 }, { 0xFF, 0x01 },
    { 0x23, 0x34 }, { 0x42, 0x00 }, { 0x44, 0xFF }, { 0x45, 0x26 }, { 0x46, 0x05 }, { 0x40, 0x40 },
    { 0x0E, 0x06 }, { 0x20, 0x1A }, { 0x43, 0x40 }, { 0xFF, 0x00 }, { 0x34, 0x03 }, { 0x35, 0x44 },
    { 0xFF, 0x01 }, { 0x31, 0x04 }, { 0x4B, 0x09 }, { 0x4C, 0x05 }, { 0x4D, 0x04 }, { 0xFF, 0x00 },
    { 0x44, 0x00 }, { 0x45, 0x20 }, { 0x47, 0x08 }, { 0x48, 0x28 }, { 0x67, 0x00 }, { 0x70, 0x04 },
    { 0x71, 0x01 }, { 0x72, 0xFE }, { 0x76, 0x00 }, { 0x77, 0x00 }, { 0xFF, 0x01 }, { 0x0D, 0x01 },
    { 0xFF, 0x00 }, { 0x80, 0x01 }, { 0x01, 0xF8 }, { 0xFF, 0x01 }, { 0x8E, 0x01 }, { 0x00, 0x01 },
    { 0xFF, 0x00 }, { 0x80, 0x00 },
};

// One chip: its bus, address, and the set-up's leftovers
class Chip {
public:
    Chip(I2cBus& bus, std::uint8_t addr) : bus_(bus), addr_(addr) {}

    bool init();
    bool readMm(std::uint16_t& mm);
    std::uint8_t stop() const { return stop_; }
    void setStop(std::uint8_t v) { stop_ = v; }

    bool w8(std::uint8_t reg, std::uint8_t v) {
        const std::uint8_t b[2] = { reg, v };
        return ok(bus_.write(addr_, b, 2));
    }
    bool w16(std::uint8_t reg, std::uint16_t v) {
        const std::uint8_t b[3] = { reg, static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v) };
        return ok(bus_.write(addr_, b, 3));
    }
    std::uint8_t r8(std::uint8_t reg) {
        std::uint8_t v = 0;
        ok(bus_.readRegs(addr_, reg, &v, 1));
        return v;
    }
    std::uint16_t r16(std::uint8_t reg) {
        std::uint8_t b[2] = { 0, 0 };
        ok(bus_.readRegs(addr_, reg, b, 2));
        return static_cast<std::uint16_t>(b[0] << 8 | b[1]);
    }
    bool failed() const { return failed_; }
    const char* step() const { return step_; }
    std::uint32_t budget() const { return budget_; }         // where it went wrong (for the log)
    void at(const char* s) { step_ = s; }

private:
    struct Enables { bool tcc, msrc, dss, preRange, finalRange; };
    struct Timeouts {
        std::uint16_t prePclks, finalPclks, msrcMclks, preMclks, finalMclks;
        std::uint32_t msrcUs, preUs, finalUs;
    };

    bool ok(bool b) { if (!b) failed_ = true; return b; }
    // Waits until (reg & mask) != 0 (or == 0 with clear); false: too long
    bool waitFor(std::uint8_t reg, std::uint8_t mask, bool clear = false) {
        for (int i = 0; i < kWaitMs; ++i) {
            const bool set = (r8(reg) & mask) != 0;
            if (failed_) return false;
            if (set != clear) return true;
            bus_.delayMs(1);
        }
        return false;
    }
    bool spadInfo(std::uint8_t& count, bool& aperture);
    bool refCalibration(std::uint8_t vhvInit);
    Enables enables();
    Timeouts timeouts(const Enables& e);
    std::uint32_t timingBudget();
    bool setTimingBudget(std::uint32_t us);

    I2cBus& bus_;
    std::uint8_t addr_;
    std::uint8_t stop_ = 0;
    bool failed_ = false;
    const char* step_ = "";
    std::uint32_t budget_ = 0;
};

std::uint16_t decodeVcsel(std::uint8_t reg) { return static_cast<std::uint16_t>((reg + 1) << 1); }
std::uint32_t macroPeriodNs(std::uint16_t pclks) { return ((2304UL * pclks * 1655UL) + 500) / 1000; }
std::uint32_t mclksToUs(std::uint16_t mclks, std::uint16_t pclks) {
    const std::uint32_t ns = macroPeriodNs(pclks);
    return ((mclks * ns) + 500) / 1000;
}
std::uint32_t usToMclks(std::uint32_t us, std::uint16_t pclks) {
    const std::uint32_t ns = macroPeriodNs(pclks);
    return ((us * 1000) + (ns / 2)) / ns;
}
std::uint16_t decodeTimeout(std::uint16_t v) {
    return static_cast<std::uint16_t>(((v & 0x00FF) << ((v & 0xFF00) >> 8)) + 1);
}
std::uint16_t encodeTimeout(std::uint32_t mclks) {
    if (mclks == 0) return 0;
    std::uint32_t ls = mclks - 1;
    std::uint16_t ms = 0;
    while ((ls & 0xFFFFFF00) > 0) { ls >>= 1; ++ms; }
    return static_cast<std::uint16_t>((ms << 8) | (ls & 0xFF));
}

bool Chip::spadInfo(std::uint8_t& count, bool& aperture) {
    w8(0x80, 0x01); w8(0xFF, 0x01); w8(0x00, 0x00);
    w8(0xFF, 0x06);
    w8(0x83, r8(0x83) | 0x04);
    w8(0xFF, 0x07); w8(0x81, 0x01);
    w8(0x80, 0x01);
    w8(0x94, 0x6B);
    w8(0x83, 0x00);
    if (!waitFor(0x83, 0xFF)) { at("SPAD info (never ready)"); return false; }
    w8(0x83, 0x01);
    const std::uint8_t t = r8(0x92);
    count = t & 0x7F;
    aperture = (t >> 7) & 0x01;
    w8(0x81, 0x00);
    w8(0xFF, 0x06);
    w8(0x83, r8(0x83) & ~0x04);
    w8(0xFF, 0x01); w8(0x00, 0x01);
    w8(0xFF, 0x00); w8(0x80, 0x00);
    return !failed_;
}

bool Chip::refCalibration(std::uint8_t vhvInit) {
    w8(SYSRANGE_START, static_cast<std::uint8_t>(0x01 | vhvInit));
    if (!waitFor(RESULT_INTERRUPT_STATUS, 0x07)) return false;
    w8(SYSTEM_INTERRUPT_CLEAR, 0x01);
    w8(SYSRANGE_START, 0x00);
    return !failed_;
}

Chip::Enables Chip::enables() {
    const std::uint8_t s = r8(SYSTEM_SEQUENCE_CONFIG);
    return { ((s >> 4) & 1) != 0, ((s >> 2) & 1) != 0, ((s >> 3) & 1) != 0, ((s >> 6) & 1) != 0, ((s >> 7) & 1) != 0 };
}

Chip::Timeouts Chip::timeouts(const Enables& e) {
    Timeouts t{};
    t.prePclks = decodeVcsel(r8(PRE_RANGE_CONFIG_VCSEL_PERIOD));
    t.msrcMclks = static_cast<std::uint16_t>(r8(MSRC_CONFIG_TIMEOUT_MACROP) + 1);
    t.msrcUs = mclksToUs(t.msrcMclks, t.prePclks);
    t.preMclks = decodeTimeout(r16(PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI));
    t.preUs = mclksToUs(t.preMclks, t.prePclks);
    t.finalPclks = decodeVcsel(r8(FINAL_RANGE_CONFIG_VCSEL_PERIOD));
    t.finalMclks = decodeTimeout(r16(FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI));
    if (e.preRange) t.finalMclks = static_cast<std::uint16_t>(t.finalMclks - t.preMclks);
    t.finalUs = mclksToUs(t.finalMclks, t.finalPclks);
    return t;
}

// Overheads of each step, microseconds (ST's numbers)
constexpr std::uint32_t kStart = 1910, kEnd = 960, kMsrc = 660, kTcc = 590, kDss = 690, kPre = 660, kFinal = 550;

std::uint32_t Chip::timingBudget() {
    const Enables e = enables();
    const Timeouts t = timeouts(e);
    std::uint32_t us = kStart + kEnd;
    if (e.tcc) us += t.msrcUs + kTcc;
    if (e.dss) us += 2 * (t.msrcUs + kDss);
    else if (e.msrc) us += t.msrcUs + kMsrc;
    if (e.preRange) us += t.preUs + kPre;
    if (e.finalRange) us += t.finalUs + kFinal;
    return us;
}

bool Chip::setTimingBudget(std::uint32_t budget) {
    if (budget < 20000) { at("timing budget (too short)"); return false; }
    const Enables e = enables();
    const Timeouts t = timeouts(e);
    std::uint32_t used = kStart + kEnd;
    if (e.tcc) used += t.msrcUs + kTcc;
    if (e.dss) used += 2 * (t.msrcUs + kDss);
    else if (e.msrc) used += t.msrcUs + kMsrc;
    if (e.preRange) used += t.preUs + kPre;
    if (e.finalRange) {
        used += kFinal;
        if (used > budget) { at("timing budget (overheads)"); return false; }
        std::uint32_t mclks = usToMclks(budget - used, t.finalPclks);
        if (e.preRange) mclks += t.preMclks;
        w16(FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI, encodeTimeout(mclks));
    }
    return !failed_;
}

bool Chip::init() {
    failed_ = false;
    at("first registers");
    // 2.8 V I/O (the modules' level), standard I2C mode
    w8(VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV, r8(VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV) | 0x01);
    w8(I2C_MODE, 0x00);
    w8(0x80, 0x01); w8(0xFF, 0x01); w8(0x00, 0x00);
    stop_ = r8(0x91);
    w8(0x00, 0x01); w8(0xFF, 0x00); w8(0x80, 0x00);
    // No SIGNAL_RATE_MSRC / SIGNAL_RATE_PRE_RANGE limit checks
    w8(MSRC_CONFIG_CONTROL, r8(MSRC_CONFIG_CONTROL) | 0x12);
    // Signal rate limit 0.25 MCPS (fixed point 9.7)
    w16(FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, static_cast<std::uint16_t>(0.25 * (1 << 7)));
    w8(SYSTEM_SEQUENCE_CONFIG, 0xFF);
    if (failed_) return false;

    // Reference SPADs: as many as the chip says, from the right first one
    std::uint8_t spadCount = 0;
    bool aperture = false;
    at("SPAD info");
    if (!spadInfo(spadCount, aperture)) return false;
    std::uint8_t map[6] = {};
    at("reference SPAD map");
    if (!ok(bus_.readRegs(addr_, GLOBAL_CONFIG_SPAD_ENABLES_REF_0, map, 6))) return false;
    w8(0xFF, 0x01);
    w8(DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00);
    w8(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C);
    w8(0xFF, 0x00);
    w8(GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4);
    const std::uint8_t first = aperture ? 12 : 0;
    std::uint8_t enabled = 0;
    for (std::uint8_t i = 0; i < 48; ++i) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1 << (i % 8));
        if (i < first || enabled == spadCount) map[i / 8] = static_cast<std::uint8_t>(map[i / 8] & ~bit);
        else if (map[i / 8] & bit) ++enabled;
    }
    std::uint8_t out[7] = { GLOBAL_CONFIG_SPAD_ENABLES_REF_0 };
    for (int i = 0; i < 6; ++i) out[i + 1] = map[i];
    ok(bus_.write(addr_, out, 7));

    at("tuning");
    for (const auto& t : kTuning) w8(t[0], t[1]);

    // "New sample ready" interrupt, active low
    w8(SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
    w8(GPIO_HV_MUX_ACTIVE_HIGH, r8(GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10);
    w8(SYSTEM_INTERRUPT_CLEAR, 0x01);
    if (failed_) return false;

    // No MSRC and TCC steps: the timing budget worked out again for that
    at("timing budget");
    const std::uint32_t budget = timingBudget();
    w8(SYSTEM_SEQUENCE_CONFIG, kSequence);
    budget_ = budget;
    if (!setTimingBudget(budget)) return false;

    // The two reference calibrations (VHV, then phase)
    w8(SYSTEM_SEQUENCE_CONFIG, 0x01);
    at("VHV calibration");
    if (!refCalibration(0x40)) return false;
    w8(SYSTEM_SEQUENCE_CONFIG, 0x02);
    at("phase calibration");
    if (!refCalibration(0x00)) return false;
    w8(SYSTEM_SEQUENCE_CONFIG, kSequence);
    return !failed_;
}

bool Chip::readMm(std::uint16_t& mm) {
    failed_ = false;
    at("measurement start");
    w8(0x80, 0x01); w8(0xFF, 0x01); w8(0x00, 0x00);
    w8(0x91, stop_);
    w8(0x00, 0x01); w8(0xFF, 0x00); w8(0x80, 0x00);
    w8(SYSRANGE_START, 0x01);
    if (failed_ || !waitFor(SYSRANGE_START, 0x01, true)) return false;      // started
    at("measurement (never done)");
    if (!waitFor(RESULT_INTERRUPT_STATUS, 0x07)) return false;              // done
    mm = r16(RESULT_RANGE_STATUS + 10);
    w8(SYSTEM_INTERRUPT_CLEAR, 0x01);
    return !failed_;
}

// Set-ups done, per address: the stop variable (to start a measurement)
std::map<std::uint8_t, std::uint8_t> ready_;

}  // namespace

bool vl53l0x_readMm(I2cBus& bus, std::uint8_t addr, double& mm) {
    Chip c(bus, addr);
    auto it = ready_.find(addr);
    // Set up when not yet, or when the chip has lost it (power cycled: the
    // sequence register is back at its reset value)
    if (it == ready_.end() || c.r8(SYSTEM_SEQUENCE_CONFIG) != kSequence) {
        ready_.erase(addr);
        if (c.failed()) {
            MLOGF("0x%02X: no answer", addr);
            return false;
        }
        if (!c.init()) {
            MLOGF("0x%02X: set-up failed at: %s%s", addr, c.step(), c.failed() ? " (no answer)" : "");
            return false;
        }
        MLOGF("0x%02X: set up (budget %lu us)", addr, static_cast<unsigned long>(c.budget()));
        it = ready_.emplace(addr, c.stop()).first;
    }
    c.setStop(it->second);
    std::uint16_t v = 0;
    if (!c.readMm(v)) {
        MLOGF("0x%02X: %s%s", addr, c.step(), c.failed() ? " (no answer)" : "");
        ready_.erase(addr);
        return false;
    }
    mm = v;
    return true;
}

void vl53l0x_reset() { ready_.clear(); }

}  // namespace hipi
