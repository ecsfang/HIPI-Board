// i2c_ident.cpp -- see i2c_ident.h. The chips themselves are described in
// i2c/*.txt on the SD card (or the built-in copy of sdcard/i2c/chips.txt);
// the format and the matching are in i2c_chips.h/.cpp. Here: reading those
// files, the bus, and the checks that need code ("probe <name>").
#define MODULE "I2C"
#include "i2c_ident.h"
#include "i2c_chips.h"
#include "i2c_command.h"
#include "ili2c.h"
#include "bmp280.h"
#include "vl53l0x.h"
#include "rtc_probe.h"        // the clock chips' checks
#include "touch.h"            // touch_i2c, TOUCH_SDA/SCL: the shared bus
#include "sd_paths.h"
#include "usb_serial.h"
#include "ff.h"
#include "hardware/i2c.h"
#include "pico/time.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

extern CBmp280* bmp280Sensor;            // the BMP280 driver for 0x76 (hipi.cpp)
extern const char* const kI2cChipsDefault;   // the built-in list (generated from sdcard/i2c/chips.txt)

namespace hipi {

namespace {

constexpr std::uint32_t kTimeoutUs = 3000;   // per transfer: a stuck chip can't hang HIPI

// ── The bus (reads only) ────────────────────────────────────────────────
struct HwBus : I2cBus {
    bool readRegs(std::uint8_t addr, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
        if (i2c_write_timeout_us(touch_i2c, addr, &reg, 1, true, kTimeoutUs) != 1) return false;
        return i2c_read_timeout_us(touch_i2c, addr, buf, n, false, kTimeoutUs) == static_cast<int>(n);
    }
    bool readRaw(std::uint8_t addr, std::uint8_t* buf, std::size_t n) override {
        return i2c_read_timeout_us(touch_i2c, addr, buf, n, false, kTimeoutUs) == static_cast<int>(n);
    }
    bool write(std::uint8_t addr, const std::uint8_t* buf, std::size_t n) override {
        return i2c_write_timeout_us(touch_i2c, addr, buf, n, false, kTimeoutUs * 4) == static_cast<int>(n);
    }
    void delayMs(int ms) override { sleep_ms(static_cast<std::uint32_t>(ms)); }
};
HwBus bus_;

// ── Checks built into the firmware ("probe <name>") ─────────────────────

// The touch panel: HIPI talks to it all the time -- answering is enough
I2cVerdict probeTouch(I2cBus&, std::uint8_t, std::string&, std::string& detail) {
    detail = "in use";
    return I2cVerdict::Ok;
}

// The BMP280 driver for a sensor on 0x76 (hipi.cpp's) or 0x77 (SDO high: a
// second one, made when first needed), ready to read -- or nullptr
CBmp280* boschAt(std::uint8_t addr) {
    static CBmp280* second = nullptr;
    CBmp280* bmp = nullptr;
    if (addr == 0x76) {
        bmp = bmp280Sensor;
    } else if (addr == 0x77) {
        if (second == nullptr) second = new CBmp280(touch_i2c, 0x77);
        bmp = second;
    }
    return bmp != nullptr && bmp->begin(TOUCH_SDA, TOUCH_SCL) ? bmp : nullptr;
}

// Bosch pressure sensors: chip ID at 0xD0. A BMP280 (0x76 or 0x77) is
// read through its driver for a reading.
I2cVerdict probeBosch(I2cBus& bus, std::uint8_t addr, std::string& name, std::string& detail) {
    std::uint8_t id = 0;
    if (!bus.readRegs(addr, 0xD0, &id, 1)) return I2cVerdict::NotIt;
    switch (id) {
        case 0x56: case 0x57: case 0x58: name = "BMP280"; break;
        case 0x60: name = "BME280"; break;
        case 0x61: name = "BME680"; break;
        case 0x55: name = "BMP180"; break;
        default: return I2cVerdict::NotIt;
    }
    char b[40];
    std::snprintf(b, sizeof(b), "ID 0x%02X", id);
    detail = b;
    CBmp280* bmp = id == 0x58 ? boschAt(addr) : nullptr;
    if (bmp != nullptr) {
        std::int32_t c = 0, pa = 0;
        if (!bmp->readTemperatureCenti(c) || !bmp->readPressurePa(pa)) {
            detail = "no reading";
            return I2cVerdict::Fault;
        }
        const bool sane = c > -4000 && c < 8500 && pa > 30000 && pa < 110000;
        std::snprintf(b, sizeof(b), "%ld.%01ld C  %ld hPa%s", static_cast<long>(c / 100),
                      static_cast<long>((c < 0 ? -c : c) % 100 / 10), static_cast<long>((pa + 50) / 100),
                      sane ? "" : "  (odd)");
        detail = b;
        return sane ? I2cVerdict::Ok : I2cVerdict::Fault;
    }
    return I2cVerdict::Ok;
}

// Measurements built into the firmware ("builtin <name>"): the Bosch
// sensor through its driver (compensated with its calibration)
bool measureBoschT(I2cBus&, std::uint8_t addr, double& v) {
    std::int32_t c = 0;
    CBmp280* bmp = boschAt(addr);
    if (bmp == nullptr || !bmp->readTemperatureCenti(c)) return false;
    v = c / 100.0;
    return true;
}
bool measureBoschP(I2cBus&, std::uint8_t addr, double& v) {
    std::int32_t pa = 0;
    CBmp280* bmp = boschAt(addr);
    if (bmp == nullptr || !bmp->readPressurePa(pa)) return false;
    v = pa / 100.0;                                      // hPa
    return true;
}

const std::map<std::string, I2cBuiltin>& builtins() {
    static const std::map<std::string, I2cBuiltin> b = {
        { "touch", probeTouch }, { "clock", rtc_probeDs }, { "pcf85063", rtc_probePcf85063 },
        { "bosch", probeBosch } };
    return b;
}

// ── The list ────────────────────────────────────────────────────────────
I2cChipList list_;
bool loaded_ = false;
std::string info_;

#ifdef DISPLAY_7INCH
constexpr int kPanel = 7;
#else
constexpr int kPanel = 5;
#endif

bool readFile(const char* path, std::string& text) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    text.clear();
    char buf[256];
    UINT got = 0;
    while (f_read(&f, buf, sizeof(buf), &got) == FR_OK && got > 0) {
        text.append(buf, got);
        if (text.size() > 64 * 1024) break;              // (not a chip list)
    }
    f_close(&f);
    return true;
}

}  // namespace

std::string i2c_loadChips() {
    list_.clear();
    vl53l0x_reset();                         // (CLRDEV: distance sensors set up again)
    // Every .txt in i2c/, in name order
    std::vector<std::string> names;
    DIR dir;
    if (f_opendir(&dir, HIPI_DIR_I2C) == FR_OK) {
        FILINFO fi;
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0] != 0) {
            if (fi.fattrib & AM_DIR) continue;
            const char* dot = std::strrchr(fi.fname, '.');
            if (dot != nullptr && (std::strcmp(dot, ".txt") == 0 || std::strcmp(dot, ".TXT") == 0)) names.push_back(fi.fname);
        }
        f_closedir(&dir);
    }
    std::sort(names.begin(), names.end());
    // HIPI's own list is built in (a firmware update brings its new chips
    // and measurements). An i2c/chips.txt on the card is an old copy of it:
    // not read -- it would hide what the firmware knows now. The card's
    // other files are read first, so a chip described there wins.
    std::vector<std::string> used;
    for (const std::string& n : names) {
        std::string lower = n;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "chips.txt") {
            LOGF("\r\n * I2C chip list: %s/%s not read -- HIPI's own list is built in (your own chips: another .txt)",
                 HIPI_DIR_I2C, n.c_str());
            continue;
        }
        std::string text;
        if (!readFile((std::string(HIPI_DIR_I2C "/") + n).c_str(), text)) continue;
        list_.parse(text, n);
        used.push_back(n);
    }
    list_.parse(kI2cChipsDefault, "built-in chips.txt");
    list_.finish(kPanel);
    for (const std::string& e : list_.errors()) LOGF("\r\n * I2C chip list: %s", e.c_str());
    // "27 descriptions  (built in)" / "(built in + i2c/my.txt)" / "(built in + i2c/my.txt and 2 others)"
    char b[96];
    const unsigned n = static_cast<unsigned>(list_.chips().size());
    if (used.empty())
        std::snprintf(b, sizeof(b), "%u descriptions  (built in)", n);
    else if (used.size() == 1)
        std::snprintf(b, sizeof(b), "%u descriptions  (built in + %s/%s)", n, HIPI_DIR_I2C, used.front().c_str());
    else
        std::snprintf(b, sizeof(b), "%u descriptions  (built in + %s/%s and %u other%s)", n, HIPI_DIR_I2C,
                      used.front().c_str(), static_cast<unsigned>(used.size() - 1), used.size() == 2 ? "" : "s");
    info_ = b;
    if (!list_.errors().empty()) info_ += ", " + std::to_string(list_.errors().size()) + " errors (see log)";
    loaded_ = true;
    return info_;
}

static const I2cChipList& chipList() {
    if (!loaded_) i2c_loadChips();
    return list_;
}

// HIPI's own chips (the touch panel): TFI2C may not write there
static bool hipiOwns(std::uint8_t addr) {
#ifdef DISPLAY_7INCH
    if (addr == 0x38) return true;                       // FT5316
#else
    if (addr == 0x40) return true;                       // GSL1680
#endif
    return chipList().expectedAt(addr) != nullptr;
}

I2cCommandEnv i2c_commandEnv() {
    I2cCommandEnv e;
    e.bus = &bus_;
    e.chips = [] () -> const I2cChipList& { return chipList(); };
    e.identifyAll = [](I2cFound* out, int max, bool found[128]) { return i2c_identify(out, max, found); };
    e.probes = builtins();
    e.measures = { { "bosch.T", measureBoschT }, { "bosch.P", measureBoschP }, { "vl53l0x.D", vl53l0x_readMm } };
    e.hipiOwns = hipiOwns;
    return e;
}

// Which file described the chip: its source without ":<line>"
static void setFile(I2cFound& f, const I2cChip* c) {
    if (c == nullptr) return;
    std::string src = c->source;
    const std::size_t colon = src.rfind(':');
    if (colon != std::string::npos) src.resize(colon);
    if (src.rfind("built-in", 0) == 0) src = "built in";
    std::snprintf(f.file, sizeof(f.file), "%s", src.c_str());
}

int i2c_identify(I2cFound* out, int max, bool found[128]) {
    if (!loaded_) i2c_loadChips();
    std::uint8_t rx;
    for (int a = 0; a < 128; ++a) found[a] = false;
    for (int a = 0x08; a <= 0x77; ++a)
        found[a] = i2c_read_timeout_us(touch_i2c, static_cast<std::uint8_t>(a), &rx, 1, false, kTimeoutUs) >= 0;
    int n = 0;
    for (int a = 0x08; a <= 0x77 && n < max; ++a) {
        I2cFound f;
        f.addr = static_cast<std::uint8_t>(a);
        if (found[a]) {
            const I2cChip* c = nullptr;
            if (!list_.identify(bus_, f.addr, f, builtins(), &c)) {
                f.state = I2cState::Unknown;
                std::snprintf(f.name, sizeof(f.name), "Unknown");
            } else {
                setFile(f, c);
            }
            out[n++] = f;
        } else if (const I2cChip* c = list_.expectedAt(f.addr)) {   // should be there, silent
            setFile(f, c);
            f.state = I2cState::Missing;
            std::snprintf(f.name, sizeof(f.name), "%s", c->name.c_str());
            std::snprintf(f.what, sizeof(f.what), "%s", c->what.c_str());
            std::snprintf(f.detail, sizeof(f.detail), "not answering");
            out[n++] = f;
        }
    }
    return n;
}

}  // namespace hipi
