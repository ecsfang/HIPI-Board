// i2ctest -- the I2C chip descriptions (src/i2c_chips.cpp): the file
// format, with sdcard/i2c/chips.txt itself, and identifying chips on a
// made-up bus. See the Makefile.
#include "i2c_chips.h"
#include "i2c_command.h"
#include "vl53l0x.h"
#include "bmp390.h"
#include "bmp180.h"
#include "bme280.h"
#include "bme680.h"
#include "ili2c.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

// What the firmware's other files provide
bool bTrace = false;
bool bExtTrace = false;
bool usb_connected = true;
std::uint64_t fakeNowUs = 1000000;

using namespace hipi;

static int failures = 0, checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// A bus with chips made of registers; remembers where a register number was written
struct FakeBus : I2cBus {
    std::map<std::uint8_t, std::map<std::uint8_t, std::uint8_t>> regs;   // addr -> reg -> value
    std::map<std::uint8_t, std::map<std::uint8_t, std::uint16_t>> regs16;   // 16-bit registers (TMP102, MCP9808, ...)
    std::map<std::uint8_t, std::uint8_t> status;                         // addr -> byte read without a register
    std::set<std::uint8_t> written;                                      // addresses a register number went to
    bool readRegs(std::uint8_t addr, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
        written.insert(addr);
        auto w = regs16.find(addr);
        if (w != regs16.end() && n == 2) {
            auto r = w->second.find(reg);
            const std::uint16_t v = r == w->second.end() ? 0xFFFF : r->second;
            buf[0] = static_cast<std::uint8_t>(v >> 8);
            buf[1] = static_cast<std::uint8_t>(v);
            return true;
        }
        auto it = regs.find(addr);
        if (it == regs.end()) return false;
        for (std::size_t i = 0; i < n; ++i) {
            auto r = it->second.find(static_cast<std::uint8_t>(reg + i));
            buf[i] = r == it->second.end() ? 0xFF : r->second;
        }
        return true;
    }
    std::map<std::uint8_t, std::vector<std::uint8_t>> raw;               // addr -> bytes read without a register
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> writes;   // what was written
    int waitedMs = 0;
    bool readRaw(std::uint8_t addr, std::uint8_t* buf, std::size_t n) override {
        auto r = raw.find(addr);
        if (r != raw.end()) {
            for (std::size_t i = 0; i < n; ++i) buf[i] = i < r->second.size() ? r->second[i] : 0xFF;
            return true;
        }
        auto it = status.find(addr);
        if (it == status.end()) return regs.count(addr) || regs16.count(addr) ? (buf[0] = 0, true) : false;
        for (std::size_t i = 0; i < n; ++i) buf[i] = it->second;
        return true;
    }
    bool write(std::uint8_t addr, const std::uint8_t* buf, std::size_t n) override {
        if (!regs.count(addr) && !regs16.count(addr) && !raw.count(addr) && !status.count(addr)) return false;
        writes.push_back({ addr, std::vector<std::uint8_t>(buf, buf + n) });
        return true;
    }
    void delayMs(int ms) override { waitedMs += ms; }
};

// A VL53L0X as the set-up sees it: registers (one page is enough here),
// the SPAD info, the reference map, a measurement that's always ready
struct VlChip : I2cBus {
    std::map<std::uint8_t, std::uint8_t> reg;
    std::uint16_t range = 300;
    bool present = true, neverReady = false;
    int writes = 0, calibrations = 0, spadsSet = -1, waited = 0;
    VlChip() {
        reg[0xC0] = 0xEE;                                // model ID
        reg[0x92] = 0x85;                                // 5 SPADs, aperture
        for (int i = 0; i < 6; ++i) reg[static_cast<std::uint8_t>(0xB0 + i)] = 0xFF;
        reg[0x01] = 0xFF;
    }
    std::uint8_t get(std::uint8_t r) {
        if (r == 0x83) return 0x10;                      // SPAD info ready
        if (r == 0x00) return 0x00;                      // a measurement has started
        if (r == 0x13) return neverReady ? 0x00 : 0x07;  // ... and is done
        if (r == 0x1E) return static_cast<std::uint8_t>(range >> 8);
        if (r == 0x1F) return static_cast<std::uint8_t>(range);
        auto it = reg.find(r);
        return it == reg.end() ? 0x00 : it->second;
    }
    bool readRegs(std::uint8_t addr, std::uint8_t r, std::uint8_t* buf, std::size_t n) override {
        if (!present || addr != 0x29) return false;
        for (std::size_t i = 0; i < n; ++i) buf[i] = get(static_cast<std::uint8_t>(r + i));
        return true;
    }
    bool readRaw(std::uint8_t addr, std::uint8_t* buf, std::size_t n) override {
        if (!present || addr != 0x29) return false;
        for (std::size_t i = 0; i < n; ++i) buf[i] = 0;
        return true;
    }
    bool write(std::uint8_t addr, const std::uint8_t* buf, std::size_t n) override {
        if (!present || addr != 0x29 || n < 2) return false;
        ++writes;
        if (buf[0] == 0xB0 && n == 7) {                  // the reference SPAD map
            spadsSet = 0;
            for (std::size_t i = 1; i < 7; ++i)
                for (int b = 0; b < 8; ++b) if (buf[i] & (1 << b)) ++spadsSet;
            if ((buf[1] | (buf[2] & 0x0F)) != 0) spadsSet = -100;   // the first 12 must be off
        }
        if (buf[0] == 0x00 && (buf[1] & 0x01) && (buf[1] & 0x40) == 0x40) ++calibrations;     // VHV
        if (buf[0] == 0x00 && buf[1] == 0x01 && reg[0x01] == 0x02) ++calibrations;          // phase
        for (std::size_t i = 1; i < n; ++i) reg[static_cast<std::uint8_t>(buf[0] + i - 1)] = buf[i];
        return true;
    }
    void delayMs(int ms) override { waited += ms; }
};

static std::uint8_t crc8(const std::vector<std::uint8_t>& d, std::size_t from, std::size_t n, std::uint8_t init) {
    std::uint8_t c = init;
    for (std::size_t i = from; i < from + n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = static_cast<std::uint8_t>(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}

static std::string readFile(const char* path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// The built-in checks, as stand-ins
static std::map<std::string, I2cBuiltin> builtins() {
    std::map<std::string, I2cBuiltin> b;
    b["touch"] = [](I2cBus&, std::uint8_t, std::string&, std::string& d) { d = "in use"; return I2cVerdict::Ok; };
    b["clock"] = [](I2cBus& bus, std::uint8_t a, std::string& n, std::string& d) {
        std::uint8_t s = 0;
        if (!bus.readRegs(a, 0x00, &s, 1) || s > 0x59) return I2cVerdict::NotIt;
        n = "DS3231";
        d = "12:00:00";
        return I2cVerdict::Ok;
    };
    b["bosch"] = [](I2cBus& bus, std::uint8_t a, std::string& n, std::string& d) {
        std::uint8_t id = 0;
        if (!bus.readRegs(a, 0xD0, &id, 1) || id != 0x58) return I2cVerdict::NotIt;
        n = "BMP280";
        d = "ID 0x58";
        return I2cVerdict::Ok;
    };
    return b;
}

static I2cFound find(const I2cChipList& list, FakeBus& bus, std::uint8_t addr) {
    I2cFound f;
    f.addr = addr;
    if (!list.identify(bus, addr, f, builtins())) {
        f.state = I2cState::Unknown;
        std::snprintf(f.name, sizeof(f.name), "Unknown");
    }
    return f;
}

static void expect(const I2cFound& f, I2cState state, const char* name, const char* detail, const char* what) {
    char b[200];
    std::snprintf(b, sizeof(b), "%s: got %s / state %d / \"%s\"", what, f.name, static_cast<int>(f.state), f.detail);
    check(f.state == state && std::string(f.name) == name && (detail == nullptr || std::string(f.detail) == detail), b);
}

// A chip for another panel (HIPI's removed 5" one), to check "panel":
// old chips.txt files on SD cards still have it
static const char* kOtherPanel =
    "chip 0x40 GSL1680 | touch panel\n"
    "  probe touch\n"
    "  expected\n"
    "  panel 5\n";

int main(int argc, char** argv) {
    const char* file = argc > 1 ? argv[1] : "../../sdcard/i2c/chips.txt";

    // ── The file that comes with HIPI ────────────────────────────────
    I2cChipList list;
    const std::string text = readFile(file);
    check(!text.empty(), std::string("read ") + file);
    list.parse(text, "chips.txt");
    for (const std::string& e : list.errors()) std::printf("  %s\n", e.c_str());
    check(list.errors().empty(), "chips.txt: no errors");
    list.parse(kOtherPanel, "other.txt");
    const std::size_t all = list.chips().size();
    list.finish(7);
    check(list.chips().size() == all - 1, "panel 7: a panel-5 chip left out");
    check(list.expectedAt(0x38) != nullptr && list.expectedAt(0x40) == nullptr, "the 7\" touch panel is expected on 0x38");
    bool checksFirst = true, seenPlain = false;
    for (const I2cChip& c : list.chips()) {
        if (!c.checks()) seenPlain = true;
        else if (seenPlain) checksFirst = false;
    }
    check(checksFirst, "chips with checks are tried first");

    // ── A made-up bus ────────────────────────────────────────────────
    FakeBus bus;
    bus.regs[0x38] = {};                                         // touch panel
    bus.regs[0x76] = { { 0xD0, 0x58 } };                         // BMP280
    bus.regs[0x29] = { { 0xC0, 0xEE } };                         // VL53L0X
    bus.regs16[0x48] = { { 0x01, 0x60A0 }, { 0x00, 0x1960 } };   // TMP102: 0x1960 -> 25.375 C
    bus.regs16[0x49] = { { 0x01, 0x8583 } };                      // ADS1115
    bus.regs16[0x18] = { { 0x06, 0x0054 }, { 0x05, 0xC1A8 } };    // MCP9808: 0x1A8 -> 26.5 C (flag bits set)
    bus.regs[0x1E] = { { 0x0A, 'H' }, { 0x0B, '4' }, { 0x0C, '3' } };   // HMC5883L
    bus.regs[0x68] = { { 0x00, 0x30 } };                         // a clock
    bus.regs[0x69] = { { 0x75, 0x70 } };                         // MPU-6500
    bus.regs[0x57] = {};                                         // EEPROM
    bus.status[0x22] = 0xFF;                                     // PCF8574 (no registers)
    bus.regs[0x42] = {};                                         // nothing known
    bus.regs16[0x4A] = { { 0x01, 0x1234 } };                     // neither ADS1115 nor TMP102 -> LM75?

    using S = I2cState;
    expect(find(list, bus, 0x38), S::Ok, "FT5316", "in use", "0x38 touch panel (probe touch)");
    expect(find(list, bus, 0x76), S::Ok, "BMP280", "ID 0x58", "0x76 BMP280 (probe bosch)");
    expect(find(list, bus, 0x29), S::Ok, "VL53L0X", "ID 0xEE", "0x29 VL53L0X (id)");
    expect(find(list, bus, 0x48), S::Ok, "TMP102", "25.4 C", "0x48 TMP102 (id16 mask, show s16 shift scale)");
    expect(find(list, bus, 0x49), S::Ok, "ADS1115/1015", "ID 0x8583", "0x49 ADS1115 (id16, renamed)");
    expect(find(list, bus, 0x18), S::Ok, "MCP9808", "26.50 C", "0x18 MCP9808 (mask, bits, before LIS3DH)");
    expect(find(list, bus, 0x1E), S::Ok, "HMC5883L", "ID H43", "0x1E HMC5883L (text)");
    expect(find(list, bus, 0x68), S::Ok, "DS3231", "12:00:00", "0x68 clock (probe clock)");
    expect(find(list, bus, 0x69), S::Ok, "MPU-6500", "ID 0x70", "0x69 MPU (id, renamed)");
    expect(find(list, bus, 0x57), S::Probable, "AT24C32", "", "0x57 EEPROM (address only)");
    expect(find(list, bus, 0x4A), S::Probable, "LM75", "", "0x4A: no ID fits -> probably LM75");
    expect(find(list, bus, 0x42), S::Probable, "INA219", "", "0x42 INA219 (address only)");
    bus.written.clear();
    expect(find(list, bus, 0x22), S::Probable, "PCF8574/MCP23017", "", "0x22 PCF8574");
    check(bus.written.count(0x22) == 0, "0x22: no register number written (a PCF8574 would take it as outputs)");
    expect(find(list, bus, 0x7A), S::Unknown, "Unknown", "", "0x7A: not in the list");

    // Out of range: fault
    bus.regs16[0x48][0x00] = 0x7F60;                             // 0x7F60 >> 4 * 0.0625 = 127.4 C
    expect(find(list, bus, 0x48), S::Fault, "TMP102", "127.4 C (odd)", "0x48 out of range -> fault");

    // ── Another file: a chip of one's own, and mistakes ─────────────
    I2cChipList two;
    two.parse(text, "chips.txt");
    two.parse(kOtherPanel, "other.txt");
    two.parse(
        "# my chips\n"
        "chip 0x5C MYSENSOR | my light sensor\n"
        "  id 0x10 0x42\n"
        "  show 0x11 u8 scale=2 unit=lx\n"
        "chip 0x90 TOOHIGH | out of range\n"       // error: address
        "  id 0x00\n"                              // error: before a chip (the bad chip was dropped)
        "chip 0x24 X\n"
        "  frobnicate 1\n"                         // error: unknown word
        "  show 0x00 f32\n",                       // error: type
        "mine.txt");
    two.finish(5);
    check(two.errors().size() == 4, "mine.txt: 4 errors noted (got " + std::to_string(two.errors().size()) + ")");
    check(!two.errors().empty() && two.errors()[0].rfind("mine.txt:5:", 0) == 0, "errors say file and line");
    check(two.expectedAt(0x40) != nullptr && two.expectedAt(0x38) == nullptr, "panel 5: its own touch panel expected, not the 7\" one");
    bus.regs[0x5C] = { { 0x10, 0x42 }, { 0x11, 21 } };
    expect(find(two, bus, 0x5C), S::Ok, "MYSENSOR", "42 lx", "0x5C my own chip goes before \"probably BH1750\"");
    bus.regs[0x5C][0x10] = 0x00;
    expect(find(two, bus, 0x5C), S::Probable, "BH1750", "", "0x5C not mine -> probably BH1750");

    // ── Measurements and I2C's commands ───────────────────────────
    {
        I2cChipList ml;
        ml.parse(text, "chips.txt");
        ml.finish(7);
        FakeBus b;
        // BMP280 on 0x76 (its measurements are built in: a stand-in)
        b.regs[0x76] = { { 0xD0, 0x58 } };
        // DS3231: 12:34:56, 26-10-11, 23.75 C
        b.regs[0x68] = { { 0x00, 0x56 }, { 0x01, 0x34 }, { 0x02, 0x12 }, { 0x04, 0x11 }, { 0x05, 0x10 }, { 0x06, 0x26 },
                         { 0x11, 23 }, { 0x12, 0xC0 } };
        // AHT20 on 0x38 (the panel-5 list: 0x38 is free): status, H = 50 %, T = 25 C
        std::vector<std::uint8_t> aht = { 0x1C, 0x80, 0x00, 0x06, 0x00, 0x00 };
        aht.push_back(crc8(aht, 0, 6, 0xFF));
        b.raw[0x38] = aht;
        // SHT3x on 0x44: T raw 0x6666 (25.0 C), H raw 0x8000 (50.0 %)
        std::vector<std::uint8_t> sht = { 0x66, 0x66, 0, 0x80, 0x00, 0 };
        sht[2] = crc8(sht, 0, 2, 0xFF);
        sht[5] = crc8(sht, 3, 2, 0xFF);
        b.raw[0x44] = sht;
        // BH1750 on 0x5C: 1200 -> 1000 lx
        b.raw[0x5C] = { 0x04, 0xB0 };
        // MPU-6050 on 0x69: AZ = 16384 (1 g)
        b.regs[0x69] = { { 0x75, 0x68 }, { 0x3F, 0x40 }, { 0x40, 0x00 } };

        I2cChipList list5;
        list5.parse(text, "chips.txt");
        list5.parse(kOtherPanel, "other.txt");
        list5.finish(5);
        auto identifyAll = [&](I2cFound* out, int max, bool found[128]) {
            int n = 0;
            for (int a = 0; a < 128; ++a) found[a] = false;
            for (int a = 0x08; a <= 0x77 && n < max; ++a) {
                if (!b.regs.count(a) && !b.raw.count(a) && !b.regs16.count(a) && !b.status.count(a)) continue;
                found[a] = true;
                I2cFound f;
                f.addr = static_cast<std::uint8_t>(a);
                if (!list5.identify(b, f.addr, f, builtins())) { f.state = I2cState::Unknown; std::snprintf(f.name, sizeof(f.name), "Unknown"); }
                out[n++] = f;
            }
            return n;
        };
        I2cCommandEnv env;
        env.bus = &b;
        env.chips = [&]() -> const I2cChipList& { return list5; };
        env.identifyAll = identifyAll;
        env.probes = builtins();
        env.measures = { { "bosch.T", [](I2cBus&, std::uint8_t a, double& v) { v = 21.37; return a == 0x76; } },
                         { "bosch.P", [](I2cBus&, std::uint8_t a, double& v) { v = 1013.27; return a == 0x76; } } };
        env.hipiOwns = [](std::uint8_t a) { return a == 0x40; };
        I2cCommander cmd(env);
        auto is = [&](const std::string& line, const std::string& want) {
            const std::string got = cmd.run(line);
            check(got == want, "\"" + line + "\" -> \"" + got + "\", want \"" + want + "\"");
        };
        // MT / MP on the BMP280
        is("A$76;MT", "21.4");
        // ',' before a letter separates commands too (easier on the HP-41)
        is("A118,MT", "21.4");
        is("A$68, R0,3", "86,52,18");
        is("A$68,R0,3,MT", "23.75");
        check(I2cCommander::readOnly("A118,MT") && !I2cCommander::readOnly("A$44,W,36,0"), "read-only lines with ','");
        is("A$76", "");
        is("MP", "1013.3");
        is("A118;P2;MT", "21.37");
        is("P;MTEMP", "21.4");
        is("M PRES", "1013.3");
        is("L", "T,P");
        is("I", "BMP280 OK");
        // Raw access
        is("A$68;R0", "86");
        is("R0,3", "86,52,18");
        is("R$11:S16", "6080");
        is("R$11:U8", "23");
        // DS3231 measurements
        is("MTIME", "12.3456");                       // HH.MMSS, as the HP-41
        is("MDATE", "2026.1011");                     // YYYY.MMDD
        is("MDMY", "11.102026");                      // the TIME module's two forms
        is("MMDY", "10.112026");
        is("MT", "23.75");
        // Measurements with writes, waits and CRC
        b.writes.clear();
        b.waitedMs = 0;
        is("A$38;MT", "25.0");
        check(!b.writes.empty() && b.writes[0].second == std::vector<std::uint8_t>({ 0xAC, 0x33, 0x00 }) && b.waitedMs == 80,
              "AHT20: trigger written, 80 ms waited");
        is("MH", "50.0");
        b.raw[0x38][6] ^= 1;
        is("MT", "ERR CRC");
        b.raw[0x38][6] ^= 1;
        is("A$44;MT", "25.0");
        is("MH", "50.0");
        is("A92;ML", "1000");
        // Setup runs once
        b.writes.clear();
        is("A$69;MAZ", "1.000");
        is("MAZ", "1.000");
        int setups = 0;
        for (const auto& w : b.writes) if (w.second == std::vector<std::uint8_t>({ 0x6B, 0x00 })) ++setups;
        check(setups == 1, "MPU-6050: setup written once");
        // The die temperature differs between the two: raw 2744 (0x0AB8) is
        // 2744/340 + 36.53 = 44.6 C on an MPU-6050, but 2744/333.87 + 21 = 29.2 C
        // on an MPU-6500 (its data sheet: 333.87 LSB/C, 0 LSB at 21 C)
        b.regs[0x69][0x41] = 0x0A;
        b.regs[0x69][0x42] = 0xB8;
        is("A$69;MT", "44.6");
        b.regs[0x69][0x75] = 0x70;                      // an MPU-6500 on the same address
        cmd.reset();
        is("A$69;MT", "29.2");
        b.regs[0x69][0x41] = 0xF8;                      // raw -2000: 15.0 C (a negative value)
        b.regs[0x69][0x42] = 0x30;
        is("A$69;MT", "15.0");
        b.regs[0x69][0x75] = 0x68;                      // back to the MPU-6050 for what follows
        cmd.reset();
        // No chip chosen: the first that has the measurement
        cmd.reset();
        is("MP", "1013.3");
        is("MLUX", "1000");
        // Writes, raw reads, waits
        b.writes.clear();
        is("A$44;W,36,0", "");
        check(b.writes.size() == 1 && b.writes[0].second == std::vector<std::uint8_t>({ 36, 0 }), "W,36,0 written as bytes");
        is("W$0D,255,1", "");
        check(b.writes.size() == 2 && b.writes[1].second == std::vector<std::uint8_t>({ 13, 255, 1 }), "W$0D,255,1: register, then bytes");
        is("Q3", std::to_string(sht[0]) + "," + std::to_string(sht[1]) + "," + std::to_string(sht[2]));
        b.waitedMs = 0;
        is("D500;D500", "");
        check(b.waitedMs == 1000, "D waits");
        is("D1500;D600", "ERR TIME");
        // Scan, names, errors
        is("S", "56,68,92,104,105,118");
        is("ADS3231;I", "DS3231 OK");
        is("ANOSUCH", "ERR NAME");
        is("A$40;W1,2", "ERR HIPI");
        is("A$42;R0", "ERR NOACK");
        is("A$68;MXYZ", "ERR NAME");
        is("A999", "ERR SYNTAX");
        is("K", "ERR SYNTAX");
        cmd.reset();
        is("R0", "ERR CHIP");
        // Which lines are run again for every reading
        check(I2cCommander::readOnly("A$76;MT") && !I2cCommander::readOnly("A$44;W,36,0"), "read-only lines");
        (void)ml;

        // ── I2C on the loop: OUTA a line, INA the answer ───────────
        int reloads = 0;
        CHipiI2c dev("I2C", env, [&] { ++reloads; });
        dev.setEnabled(true);
        dev.hpil(AAD + 1);
        auto outa = [&](const std::string& line) {
            dev.hpil(LAD + 1);
            for (char c : line) dev.hpil(static_cast<IL_CMD_t>(static_cast<std::uint8_t>(c)));
            dev.hpil('\r');
            dev.hpil('\n');
            dev.hpil(UNL);
        };
        auto ina = [&]() {
            dev.hpil(TAD + 1);
            std::string got;
            IL_CMD_t f = dev.hpil(SDA);
            for (int i = 0; i < 40 && f != ETO; ++i) {
                got += static_cast<char>(f & 0xFF);
                f = dev.hpil(f);                         // the byte back round the loop
            }
            dev.hpil(UNT);
            return got;
        };
        check(dev.accessoryId() == 0x52, "I2C: accessory ID 0x52");
        outa("A$76;MT");
        check(ina() == "21.4\r\n", "OUTA \"A$76;MT\", INA -> 21.4");
        b.raw[0x5C] = { 0x00, 0x60 };                    // (another chip: unchanged answer)
        check(ina() == "21.4\r\n", "INA again: measured again");
        outa("A92;ML");
        b.raw[0x5C] = { 0x04, 0xB0 };
        check(ina() == "1000\r\n", "INA: measured at the reading (1000 lx)");
        b.raw[0x5C] = { 0x09, 0x60 };
        check(ina() == "2000\r\n", "INA again: a fresh value (2000 lx)");
        b.writes.clear();
        outa("A$44;W,36,0");
        check(b.writes.size() == 1, "a line with W: written at once");
        ina();
        ina();
        check(b.writes.size() == 1, "... and not again at each INA");
        // A line with only A / P: done at once -- the next line uses it
        outa("A$76");
        outa("MT");
        check(ina() == "21.4\r\n", "OUTA \"A$76\", OUTA \"MT\", INA -> 21.4 (the A done at once)");
        outa("A$68");
        outa("P1");
        outa("MT");
        check(ina() == "23.8\r\n", "A, then P1, then MT on separate lines -> 23.8");
        check(I2cCommander::settingsOnly("A$29") && I2cCommander::settingsOnly("A118,P2") &&
              !I2cCommander::settingsOnly("A118,MT") && !I2cCommander::settingsOnly(""), "settings-only lines");
        outa("P");
        outa("A$42;R0");
        check(ina() == "ERR NOACK\r\n", "errors are answered");
        dev.hpil(DCL);
        check(reloads == 1, "DCL: the chip descriptions read again");
        outa("R0");
        check(ina() == "ERR CHIP\r\n", "DCL: no chip chosen any more");
    }

    // ── VL53L0X: HIPI sets it up, the HP-41 only asks "MD" ───────────────
    {
        I2cChipList vl;
        vl.parse(text, "chips.txt");
        vl.finish(7);
        VlChip chip;
        I2cCommandEnv env;
        env.bus = &chip;
        env.chips = [&]() -> const I2cChipList& { return vl; };
        env.identifyAll = [&](I2cFound* out, int max, bool found[128]) {
            for (int a = 0; a < 128; ++a) found[a] = chip.present && a == 0x29;
            if (!chip.present || max < 1) return 0;
            I2cFound f;
            f.addr = 0x29;
            vl.identify(chip, 0x29, f, builtins());
            out[0] = f;
            return 1;
        };
        env.probes = builtins();
        env.measures = { { "vl53l0x.D", vl53l0x_readMm } };
        env.hipiOwns = [](std::uint8_t) { return false; };
        I2cCommander cmd(env);
        auto is = [&](const std::string& line, const std::string& want) {
            const std::string got = cmd.run(line);
            check(got == want, "\"" + line + "\" -> \"" + got + "\", want \"" + want + "\"");
        };
        vl53l0x_reset();
        is("AVL53L0X;MD", "300");
        const int setup = chip.writes;
        check(setup > 80 && chip.reg[0x01] == 0xE8, "VL53L0X: set up (tuning, sequence 0xE8)");
        check(chip.reg[0x80] == 0x00 && chip.reg[0xFF] == 0x00, "... and back on the normal register page");
        check(chip.spadsSet == 5, "... 5 reference SPADs, from the 13th (aperture)");
        check(chip.calibrations == 2, "... VHV and phase calibration");
        chip.writes = 0;
        chip.range = 1234;
        is("MDIST", "1234");
        check(chip.writes > 0 && chip.writes < 15, "the next reading: no set-up again");
        chip.reg[0x01] = 0xFF;                           // power cycled: lost its set-up
        chip.writes = 0;
        is("MD", "1234");
        check(chip.writes > 80, "set up again after a power cycle");
        chip.range = 8190;
        is("MD", "8190");
        chip.present = false;
        is("MD", "ERR NOACK");
        chip.present = true;
        chip.reg[0x01] = 0xFF;                           // (plugged in again: a fresh chip)
        chip.writes = 0;
        chip.range = 42;
        is("MD", "42");
        check(chip.writes > 80, "back after unplugging: set up again");
        chip.neverReady = true;
        is("MD", "ERR NOACK");
        check(chip.waited >= 100, "a measurement that never finishes: given up after 100 ms");
    }

    // ── BMP390: compensated with the chip's own calibration ──────────────
    {
        // A made-up BMP390: its NVM data, and the raw values a forced
        // measurement gives (made so that it is 22.00 C and 1013.25 hPa --
        // worked out separately, with the datasheet's formulas)
        struct Bmp390Chip : I2cBus {
            std::uint8_t id = 0x60;
            bool present = true, neverReady = false;
            bool forced = false;
            int waited = 0;
            std::uint32_t rawT = 8534164, rawP = 6183188;
            std::uint8_t calib[21] = { 0x1e, 0x6e, 0x8c, 0x46, 0xe6, 0x00, 0x7d, 0xbc, 0x42, 0x01, 0xf7,
                                       0xe8, 0x03, 0xb8, 0x0b, 0x07, 0xf8, 0x14, 0x00, 0x0c, 0xfe };
            bool readRegs(std::uint8_t a, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
                if (!present || a != 0x76) return false;
                for (std::size_t i = 0; i < n; ++i) {
                    const unsigned r = reg + i;
                    std::uint8_t v = 0;
                    if (r == 0x00) v = id;
                    else if (r == 0x03) v = forced && !neverReady && waited >= 20 ? 0x70 : 0x10;
                    else if (r >= 0x04 && r <= 0x06) v = static_cast<std::uint8_t>(rawP >> (8 * (r - 0x04)));
                    else if (r >= 0x07 && r <= 0x09) v = static_cast<std::uint8_t>(rawT >> (8 * (r - 0x07)));
                    else if (r >= 0x31 && r < 0x31 + 21) v = calib[r - 0x31];
                    buf[i] = v;
                }
                return true;
            }
            bool readRaw(std::uint8_t, std::uint8_t*, std::size_t) override { return false; }
            bool write(std::uint8_t a, const std::uint8_t* b, std::size_t n) override {
                if (!present || a != 0x76 || n != 2) return false;
                if (b[0] == 0x1B && (b[1] & 0x30) == 0x10 && (b[1] & 0x03) == 0x03) { forced = true; waited = 0; }
                if (b[0] == 0x1C) osr = b[1];
                return true;
            }
            std::uint8_t osr = 0;
            void delayMs(int ms) override { waited += ms; }
        };
        Bmp390Chip chip;
        double t = 0, p = 0;
        check(bmp390_read(chip, 0x76, t, p), "BMP390: a reading");
        check(std::fabs(t - 22.0) < 0.001, "... 22.00 C (got " + std::to_string(t) + ")");
        check(std::fabs(p - 101325.0) < 0.5, "... 1013.25 hPa (got " + std::to_string(p / 100.0) + ")");
        check(chip.osr == 0x03, "... pressure x8, temperature x1");
        chip.rawT = 8334164;                             // 200000 counts lower: 18.68 C and 1011.36 hPa
        chip.forced = false;
        check(bmp390_read(chip, 0x76, t, p) && std::fabs(t - 18.68104) < 0.001 && std::fabs(p - 101136.05) < 0.5,
              "a cooler reading: the pressure follows the temperature");
        chip.id = 0x50;                                  // a BMP388: the same formulas
        chip.forced = false;
        check(bmp390_read(chip, 0x76, t, p), "a BMP388 (ID 0x50): the same");
        chip.id = 0x58;                                  // a BMP280 at the same address
        check(!bmp390_read(chip, 0x76, t, p), "another chip (ID 0x58): no reading");
        chip.id = 0x60;
        chip.forced = false;
        chip.neverReady = true;
        check(!bmp390_read(chip, 0x76, t, p) && chip.waited <= 130, "a measurement that never finishes: given up");
        chip.neverReady = false;
        chip.present = false;
        check(!bmp390_read(chip, 0x76, t, p), "not answering: no reading");
        chip.present = true;
        chip.forced = false;

        // Identified by the ID, and I2C's "MT" / "MP" give the numbers
        I2cChipList bl;
        bl.parse(text, "chips.txt");
        bl.finish(7);
        I2cCommandEnv env;
        env.bus = &chip;
        env.chips = [&]() -> const I2cChipList& { return bl; };
        env.identifyAll = [&](I2cFound* out, int max, bool found[128]) {
            for (int a = 0; a < 128; ++a) found[a] = a == 0x76;
            if (max < 1) return 0;
            I2cFound f;
            f.addr = 0x76;
            bl.identify(chip, 0x76, f, builtins());
            out[0] = f;
            return 1;
        };
        env.probes = builtins();
        env.measures = { { "bmp390.T", [](I2cBus& b, std::uint8_t a, double& v) { double x, y; bool ok = bmp390_read(b, a, x, y); v = x; return ok; } },
                         { "bmp390.P", [](I2cBus& b, std::uint8_t a, double& v) { double x, y; bool ok = bmp390_read(b, a, x, y); v = y / 100.0; return ok; } } };
        env.hipiOwns = [](std::uint8_t) { return false; };
        I2cCommander cmd(env);
        I2cFound f;
        check(bl.identify(chip, 0x76, f, builtins()) && std::string(f.name) == "BMP390", "0x76: identified as BMP390");
        chip.id = 0x50;
        I2cFound f2;
        check(bl.identify(chip, 0x76, f2, builtins()) && std::string(f2.name) == "BMP388", "... ID 0x50: BMP388");
        chip.id = 0x60;
        auto is = [&](const std::string& line, const std::string& want) {
            const std::string got = cmd.run(line);
            check(got == want, "\"" + line + "\" -> \"" + got + "\", want \"" + want + "\"");
        };
        chip.rawT = 8534164;                             // (back to 22.00 C)
        chip.forced = false;
        is("A0x76;MT", "22.00");
        chip.forced = false;
        is("MP", "1013.25");
    }

    // ── BMP180 / BMP085: the datasheet's own worked example ──────────────
    {
        const std::uint8_t calib[22] = { 0x01, 0x98, 0xff, 0xb8, 0xc7, 0xd1, 0x7f, 0xe5, 0x7f, 0xf5, 0x5a,
                                         0x71, 0x18, 0x2e, 0x00, 0x04, 0x80, 0x00, 0xdd, 0xf9, 0x0b, 0x34 };
        // AC1=408 AC2=-72 AC3=-14383 AC4=32741 AC5=32757 AC6=23153 B1=6190 B2=4 MB=-32768 MC=-8711 MD=2868
        std::int32_t t = 0, p = 0;
        bmp180_compensate(calib, 27898, 23843, 0, t, p);
        check(t == 150 && p == 69964, "BMP180 datasheet example (oss 0): 15.0 C, 69964 Pa (got " + std::to_string(t) +
                                          ", " + std::to_string(p) + ")");

        // A made-up BMP180 with that calibration; the raw pressure is made so that it is 1013.25 hPa at oss 3
        struct Bmp180Chip : I2cBus {
            std::uint8_t id = 0x55;
            bool present = true;
            std::uint8_t ctrl = 0;
            std::uint8_t calib[22];
            std::int32_t ut = 27898, up = 274643;       // up: after the shift, i.e. what the oss gives
            bool readRegs(std::uint8_t a, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
                if (!present || a != 0x77) return false;
                for (std::size_t i = 0; i < n; ++i) {
                    const unsigned r = reg + i;
                    std::uint8_t v = 0;
                    if (r == 0xD0) v = id;
                    else if (r >= 0xAA && r < 0xAA + 22) v = calib[r - 0xAA];
                    else if (r >= 0xF6 && r <= 0xF8) {
                        std::int32_t raw = ctrl == 0x2E ? ut << 8 : up << (8 - ((ctrl >> 6) & 3));   // 24 bits
                        v = static_cast<std::uint8_t>(raw >> (8 * (2 - (r - 0xF6))));
                    }
                    buf[i] = v;
                }
                return true;
            }
            bool readRaw(std::uint8_t, std::uint8_t*, std::size_t) override { return false; }
            bool write(std::uint8_t a, const std::uint8_t* b, std::size_t n) override {
                if (!present || a != 0x77 || n != 2) return false;
                if (b[0] == 0xF4) ctrl = b[1];
                return true;
            }
            void delayMs(int) override {}
        };
        Bmp180Chip chip;
        std::memcpy(chip.calib, calib, 22);
        double tc = 0, pa = 0;
        check(bmp180_read(chip, 0x77, tc, pa), "BMP180: a reading");
        check(std::fabs(tc - 15.0) < 0.001 && std::fabs(pa - 101325.0) < 0.5,
              "... 15.0 C, 1013.25 hPa (got " + std::to_string(tc) + ", " + std::to_string(pa / 100.0) + ")");
        check(chip.ctrl == 0xF4, "... the pressure at oss 3");
        chip.id = 0x58;
        check(!bmp180_read(chip, 0x77, tc, pa), "another chip (ID 0x58): no reading");
        chip.id = 0x55;
        std::memset(chip.calib, 0xFF, 22);
        check(!bmp180_read(chip, 0x77, tc, pa), "calibration unreadable (0xFFFF): no reading");
        std::memcpy(chip.calib, calib, 22);
        chip.present = false;
        check(!bmp180_read(chip, 0x77, tc, pa), "not answering: no reading");
        chip.present = true;

        // The chip list takes ID 0x55 as a BMP180 (not through the "bosch" probe), and MT / MP give the numbers
        I2cChipList bl;
        bl.parse(text, "chips.txt");
        bl.finish(7);
        I2cFound f;
        check(bl.identify(chip, 0x77, f, builtins()) && std::string(f.name) == "BMP180", "0x77, ID 0x55: identified as BMP180");
    }

    // ── BME280: Bosch's own temperature/pressure example, and a humidity ──
    {
        // The calibration of Bosch's BMP280 example (the BME280 calculates the same) and a made-up humidity one.
        // Worked out separately, with the datasheet's integer formulas (the humidity also with its float ones):
        // adc 519888 / 415148 / 30000 gives 25.08 C, 100653.25 Pa, 53.93 %RH
        std::uint8_t tp[24] = { 0x70, 0x6b, 0x43, 0x67, 0x18, 0xfc, 0x7d, 0x8e, 0x43, 0xd6, 0xd0, 0x0b,
                                0x27, 0x0b, 0x8c, 0x00, 0xf9, 0xff, 0x8c, 0x3c, 0xf8, 0xc6, 0x70, 0x17 };
        std::uint8_t h[7] = { 0x6a, 0x01, 0x00, 0x13, 0x2c, 0x03, 0x1e };   // H2=362 H3=0 H4=316 H5=50 H6=30 (H1=75 at 0xA1)
        double t = 0, p = 0, rh = 0;
        bme280_compensate(tp, 75, h, 519888, 415148, 30000, t, p, rh);
        check(std::fabs(t - 25.08) < 0.001 && std::fabs(p - 100653.25) < 0.01 && std::fabs(rh - 53.93) < 0.01,
              "BME280 compensation: 25.08 C, 100653.25 Pa, 53.93 %RH (got " + std::to_string(t) + ", " +
                  std::to_string(p) + ", " + std::to_string(rh) + ")");
        bme280_compensate(tp, 75, h, 519888, 415148, 0, t, p, rh);
        check(rh == 0.0, "... humidity never below 0");

        struct Bme280Chip : I2cBus {
            std::uint8_t id = 0x60;
            bool present = true, measuring = false;
            int waited = 0;
            std::uint8_t ctrlHum = 0, ctrlMeas = 0;
            std::uint8_t tp[24], h1 = 75, h[7];
            std::uint8_t data[8] = { 0x65, 0x5a, 0xc0, 0x7e, 0xed, 0x00, 0x75, 0x30 };   // the raw values above
            bool readRegs(std::uint8_t a, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
                if (!present || (a != 0x76 && a != 0x77)) return false;
                for (std::size_t i = 0; i < n; ++i) {
                    const unsigned r = reg + i;
                    std::uint8_t v = 0;
                    if (r == 0xD0) v = id;
                    else if (r >= 0x88 && r < 0x88 + 24) v = tp[r - 0x88];
                    else if (r == 0xA1) v = h1;
                    else if (r >= 0xE1 && r < 0xE1 + 7) v = h[r - 0xE1];
                    else if (r == 0xF3) v = measuring && waited < 18 ? 0x08 : 0x00;
                    else if (r >= 0xF7 && r <= 0xFE) v = data[r - 0xF7];
                    buf[i] = v;
                }
                return true;
            }
            bool readRaw(std::uint8_t, std::uint8_t*, std::size_t) override { return false; }
            bool write(std::uint8_t a, const std::uint8_t* b, std::size_t n) override {
                if (!present || n != 2) return false;
                if (b[0] == 0xF2) ctrlHum = b[1];
                if (b[0] == 0xF4) { ctrlMeas = b[1]; measuring = true; waited = 0; }
                (void)a;
                return true;
            }
            void delayMs(int ms) override { waited += ms; }
        };
        Bme280Chip chip;
        std::memcpy(chip.tp, tp, 24);
        std::memcpy(chip.h, h, 7);
        check(bme280_read(chip, 0x76, t, p, rh), "BME280: a reading");
        check(std::fabs(t - 25.08) < 0.001 && std::fabs(p - 100653.25) < 0.01 && std::fabs(rh - 53.93) < 0.01,
              "... 25.08 C, 1006.53 hPa, 53.93 %RH (got " + std::to_string(t) + ", " + std::to_string(p / 100.0) + ", " +
                  std::to_string(rh) + ")");
        check(chip.ctrlHum == 1 && chip.ctrlMeas == 0x4D, "... humidity x1, temperature x2, pressure x4, forced mode");
        check(bme280_read(chip, 0x77, t, p, rh), "... also on 0x77");
        chip.id = 0x58;                                  // a BMP280
        check(!bme280_read(chip, 0x76, t, p, rh), "another chip (ID 0x58): no reading");
        chip.id = 0x60;
        chip.present = false;
        check(!bme280_read(chip, 0x76, t, p, rh), "not answering: no reading");
        chip.present = true;

        // Identified by ID 0x60 (not through the "bosch" probe)
        I2cChipList bl;
        bl.parse(text, "chips.txt");
        bl.finish(7);
        I2cFound f;
        check(bl.identify(chip, 0x76, f, builtins()) && std::string(f.name) == "BME280", "0x76, ID 0x60: identified as BME280");
    }

    // ── BME680 / BME688: temperature, pressure, humidity (no gas) ────────
    {
        // Made-up but typical calibration; the raw values were worked out so that it is 22.00 C, 1013.25 hPa and 50 %RH
        // (separately, with the datasheet's float formulas; its integer ones agree within 3 Pa and 0.02 %RH)
        const std::uint8_t a[23] = { 0x46, 0x67, 0x03, 0x00, 0xab, 0x8f, 0xf4, 0xd6, 0x58, 0x00, 0xc4, 0x1b, 0x42, 0xff, 0x2c, 0x1e, 0x00, 0x00, 0x02, 0xf9, 0x7c, 0xfb, 0x1e };
        const std::uint8_t b[10] = { 0x3f, 0x79, 0x2e, 0x00, 0x2d, 0x14, 0x78, 0x9c, 0x0d, 0x66 };
        const std::uint8_t data[10] = { 0x80, 0x00, 0x52, 0xb6, 0x00, 0x77, 0x17, 0x50, 0x54, 0x44 };
        double t = 0, p = 0, rh = 0;
        bme680_compensate(a, b, 487797, 338784, 21572, t, p, rh);
        check(std::fabs(t - 22.0) < 0.01 && std::fabs(p - 101324.9) < 0.5 && std::fabs(rh - 50.0) < 0.05,
              "BME680 compensation: 22.00 C, 101324.9 Pa, 50.0 %RH (got " + std::to_string(t) + ", " + std::to_string(p) +
                  ", " + std::to_string(rh) + ")");

        struct Bme680Chip : I2cBus {
            std::uint8_t id = 0x61;
            bool present = true, measuring = false;
            int waited = 0;
            std::uint8_t ctrlGas = 0xFF, ctrlHum = 0, ctrlMeas = 0;
            std::uint8_t a[23], b[10], data[10];
            bool readRegs(std::uint8_t ad, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
                if (!present || (ad != 0x76 && ad != 0x77)) return false;
                for (std::size_t i = 0; i < n; ++i) {
                    const unsigned r = reg + i;
                    std::uint8_t v = 0;
                    if (r == 0xD0) v = id;
                    else if (r >= 0x8A && r < 0x8A + 23) v = a[r - 0x8A];
                    else if (r >= 0xE1 && r < 0xE1 + 10) v = b[r - 0xE1];
                    else if (r == 0x1D) v = !measuring ? 0x00 : waited < 28 ? 0x20 : 0x80;
                    else if (r >= 0x1E && r <= 0x26) v = data[r - 0x1D];
                    buf[i] = v;
                }
                return true;
            }
            bool readRaw(std::uint8_t, std::uint8_t*, std::size_t) override { return false; }
            bool write(std::uint8_t ad, const std::uint8_t* bb, std::size_t n) override {
                if (!present || n != 2) return false;
                if (bb[0] == 0x71) ctrlGas = bb[1];
                if (bb[0] == 0x72) ctrlHum = bb[1];
                if (bb[0] == 0x74) { ctrlMeas = bb[1]; measuring = true; waited = 0; }
                (void)ad;
                return true;
            }
            void delayMs(int ms) override { waited += ms; }
        };
        Bme680Chip chip;
        std::memcpy(chip.a, a, 23);
        std::memcpy(chip.b, b, 10);
        std::memcpy(chip.data, data, 10);
        check(bme680_read(chip, 0x77, t, p, rh), "BME680: a reading");
        check(std::fabs(t - 22.0) < 0.01 && std::fabs(p - 101324.9) < 0.5 && std::fabs(rh - 50.0) < 0.05,
              "... 22.00 C, 1013.25 hPa, 50.0 %RH (got " + std::to_string(t) + ", " + std::to_string(p / 100.0) + ", " +
                  std::to_string(rh) + ")");
        check(chip.ctrlGas == 0x00 && chip.ctrlHum == 1 && chip.ctrlMeas == 0x4D, "... no gas, humidity x1, temperature x2, pressure x4, forced");
        chip.id = 0x60;                                  // a BME280
        check(!bme680_read(chip, 0x77, t, p, rh), "another chip (ID 0x60): no reading");
        chip.id = 0x61;
        chip.present = false;
        check(!bme680_read(chip, 0x77, t, p, rh), "not answering: no reading");
        chip.present = true;

        I2cChipList bl;
        bl.parse(text, "chips.txt");
        bl.finish(7);
        I2cFound f;
        check(bl.identify(chip, 0x77, f, builtins()) && std::string(f.name) == "BME680", "0x77, ID 0x61: identified as BME680");
    }

    std::printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
