// rtc_test.cpp -- the real-time clock chips on the PC: rtc.cpp (finding
// the chip, reading and setting the time), rtc_probe.cpp (the Hardware
// test page's checks) and the built-in chip list's entries (sdcard/i2c/
// chips.txt: identifying, TFI2C's TIME/DATE measurements), against
// made-up chips on a made-up bus: DS3231, DS1307, PCF85063A -- and a
// PCF8563, which sits on the PCF85063A's address but isn't one.
//
//   make rtc_test && ./rtc_test   (in tools/hipiview)
#include "rtc.h"
#include "rtc_probe.h"
#include "i2c_chips.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>

// ── The made-up bus ────────────────────────────────────────────────────
enum class Kind { DS3231, DS1307, PCF85063A, PCF8563, EEPROM };

// PCF85063A (datasheet tables 5 and 8): the bits it doesn't have, and the
// registers after a reset
const std::uint8_t kPcfUnused[0x12] = { 0x48, 0, 0, 0, 0, 0x80, 0xC0, 0xC0, 0xF8,
                                        0xE0, 0, 0, 0, 0x40, 0x40, 0x78, 0, 0xE0 };
const std::uint8_t kPcfReset[0x12] = { 0, 0, 0, 0, 0x80, 0, 0, 0x01, 0x06,
                                       0x01, 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0, 0x18 };

struct FakeChip {
    Kind kind;
    std::uint8_t regs[256] = {};
    std::uint8_t ptr = 0;
    // Writes that don't stick: DS3231 0x11/0x12 (temperature, read-only)
    bool readOnly(std::uint8_t r) const { return kind == Kind::DS3231 && (r == 0x11 || r == 0x12); }
    // PCF85063A: a 5-bit register address, 0x00..0x11 then back to 0x00
    std::uint8_t index() const { return kind == Kind::PCF85063A ? static_cast<std::uint8_t>(ptr & 0x1F) : ptr; }
    void next() {
        ++ptr;
        if (kind == Kind::PCF85063A && (ptr & 0x1F) >= 0x12) ptr = 0;
    }
    void store(std::uint8_t v) {
        const std::uint8_t r = index();
        if (readOnly(r)) return;
        if (kind == Kind::PCF85063A) {
            if (r == 0x00 && v == 0x58) {                    // software reset
                for (int i = 0; i < 0x12; ++i) regs[i] = kPcfReset[i];
                return;
            }
            v = static_cast<std::uint8_t>(v & ~kPcfUnused[r]);
        }
        regs[r] = v;
    }
};

std::map<std::uint8_t, FakeChip> chips;
std::uint64_t timeUs = 0;
std::uint64_t& fakeTimeUs() { return timeUs; }

int fakeI2cWrite(std::uint8_t addr, const std::uint8_t* src, std::size_t len) {
    auto it = chips.find(addr);
    if (it == chips.end()) return PICO_ERROR_GENERIC;                // no ACK
    FakeChip& c = it->second;
    if (len == 0) return 0;
    c.ptr = src[0];
    if (c.kind == Kind::PCF85063A && (c.ptr & 0x1F) >= 0x12) c.ptr = 0;
    for (std::size_t i = 1; i < len; ++i, c.next()) c.store(src[i]);
    return static_cast<int>(len);
}

int fakeI2cRead(std::uint8_t addr, std::uint8_t* dst, std::size_t len) {
    auto it = chips.find(addr);
    if (it == chips.end()) return PICO_ERROR_GENERIC;
    FakeChip& c = it->second;
    for (std::size_t i = 0; i < len; ++i, c.next()) dst[i] = c.regs[c.index()];
    return static_cast<int>(len);
}

// The same chips for the chip list and the probes
struct Bus : hipi::I2cBus {
    bool readRegs(std::uint8_t addr, std::uint8_t reg, std::uint8_t* buf, std::size_t n) override {
        return fakeI2cWrite(addr, &reg, 1) == 1 && fakeI2cRead(addr, buf, n) == static_cast<int>(n);
    }
    bool readRaw(std::uint8_t addr, std::uint8_t* buf, std::size_t n) override {
        return fakeI2cRead(addr, buf, n) == static_cast<int>(n);
    }
    bool write(std::uint8_t addr, const std::uint8_t* buf, std::size_t n) override {
        return fakeI2cWrite(addr, buf, n) == static_cast<int>(n);
    }
    void delayMs(int ms) override { timeUs += static_cast<std::uint64_t>(ms) * 1000; }
};

// A chip as it is after power-up (time lost)
void addChip(std::uint8_t addr, Kind k) {
    FakeChip c{ k };
    switch (k) {
        case Kind::DS3231:
            c.regs[0x04] = 0x01; c.regs[0x05] = 0x01;
            c.regs[0x0F] = 0x80;                     // OSF
            c.regs[0x11] = 0x18; c.regs[0x12] = 0x40; // 24.25 C
            break;
        case Kind::DS1307:
            c.regs[0x00] = 0x80;                     // CH
            c.regs[0x04] = 0x01; c.regs[0x05] = 0x01;
            break;
        case Kind::PCF85063A:                        // datasheet reset values (OS set)
            for (int i = 0; i < 0x12; ++i) c.regs[i] = kPcfReset[i];
            break;
        case Kind::EEPROM:                           // something stored in it
            for (int i = 0; i < 256; ++i) c.regs[i] = static_cast<std::uint8_t>(i * 37 + 11);
            break;
        case Kind::PCF8563:                          // 2026-10-17 19:47:30 in its own layout
            c.regs[0x02] = 0x30; c.regs[0x03] = 0x47; c.regs[0x04] = 0x19;
            c.regs[0x05] = 0x17; c.regs[0x06] = 0x06; c.regs[0x07] = 0x10; c.regs[0x08] = 0x26;
            break;
    }
    chips[addr] = c;
}

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

bool same(const hipi::DateTime& a, const hipi::DateTime& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour &&
           a.minute == b.minute && a.second == b.second;
}

hipi::DateTime when(int y, int mo, int d, int h, int mi, int s) {
    hipi::DateTime t;
    t.year = y; t.month = mo; t.day = d; t.hour = h; t.minute = mi; t.second = s;
    return t;
}

i2c_inst_t bus0{ 0 };

int main() {
    // Run from tools/hipiview: the project root is two up
    if (chdir("../..") != 0) { std::perror("chdir"); return 1; }
    using hipi::CRtc;
    using hipi::RtcChip;

    // ── PCF85063A alone ────────────────────────────────────────────────
    chips.clear();
    addChip(0x51, Kind::PCF85063A);
    {
        CRtc rtc(&bus0);
        check(rtc.begin(16, 17), "PCF85063A: found");
        check(rtc.chip() == RtcChip::PCF85063A && std::strcmp(rtc.chipName(), "PCF85063A") == 0, "PCF85063A: named");
        check(rtc.address() == 0x51, "PCF85063A: on 0x51");
        check(!rtc.valid(), "PCF85063A: after power-up the time isn't valid (OS)");
        const hipi::DateTime t = when(2026, 10, 17, 19, 47, 30);
        check(rtc.set(t), "PCF85063A: set");
        check(rtc.valid(), "PCF85063A: valid after set");
        const std::uint8_t* r = chips[0x51].regs;
        check(r[0x04] == 0x30 && r[0x05] == 0x47 && r[0x06] == 0x19, "PCF85063A: time registers (BCD, 24 h)");
        check(r[0x07] == 0x17 && r[0x08] == 6 && r[0x09] == 0x10 && r[0x0A] == 0x26,
              "PCF85063A: day, weekday (Saturday = 6), month, year");
        hipi::DateTime back;
        check(rtc.read(back) && same(back, t) && back.weekday == 6, "PCF85063A: read back");
    }
    // 12-hour mode and STOP (set by someone else): read right, set() clears both
    {
        FakeChip& c = chips[0x51];
        c.regs[0x00] = 0x22 | 0x01;          // STOP, 12_24, CAP_SEL
        c.regs[0x06] = 0x20 | 0x07;          // 7 PM
        CRtc rtc(&bus0);
        rtc.begin(16, 17);
        hipi::DateTime t;
        check(rtc.read(t) && t.hour == 19, "PCF85063A: 12-hour mode read (7 PM = 19)");
        check(!rtc.valid(), "PCF85063A: STOP = not valid");
        check(rtc.set(when(2026, 1, 4, 23, 5, 9)), "PCF85063A: set again");
        check(c.regs[0x00] == 0x01, "PCF85063A: set() -> 24 h, running; CAP_SEL left alone");
        check(c.regs[0x06] == 0x23 && c.regs[0x08] == 0 && rtc.valid(), "PCF85063A: 23 h, Sunday = 0, valid");
    }

    // Garbled registers (powered up from a residual voltage, plugged in
    // while powered: datasheet 8.2.1.3): no time in them, but still a
    // PCF85063A; setting the time starts with a software reset
    chips.clear();
    addChip(0x51, Kind::PCF85063A);
    {
        const std::uint8_t garbled[0x12] = { 0x81, 0x5A, 0x7F, 0xEE, 0x7A, 0x7F, 0x3F, 0x3F, 0x07,
                                             0x1F, 0xFF, 0x00, 0x91, 0xBF, 0xBF, 0x87, 0x42, 0x1F };
        FakeChip& c = chips[0x51];
        for (int i = 0; i < 0x12; ++i) c.regs[i] = static_cast<std::uint8_t>(garbled[i] & ~kPcfUnused[i]);
        CRtc rtc(&bus0);
        check(rtc.begin(16, 17) && rtc.chip() == RtcChip::PCF85063A, "PCF85063A, garbled registers: found (fingerprint)");
        check(!rtc.valid(), "PCF85063A, garbled registers: not valid");
        const hipi::DateTime t = when(2026, 10, 18, 12, 10, 0);
        hipi::DateTime back;
        check(rtc.set(t) && rtc.valid() && rtc.read(back) && same(back, t), "PCF85063A, garbled registers: set -> valid");
        check(c.regs[0x00] == 0x00 && c.regs[0x01] == 0x00 && c.regs[0x11] == 0x18,
              "PCF85063A, garbled registers: software reset first (EXT_TEST, CAP_SEL, timer back to reset values)");
        std::string name, detail;
        Bus b;
        for (int i = 0; i < 0x12; ++i) c.regs[i] = static_cast<std::uint8_t>(garbled[i] & ~kPcfUnused[i]);
        check(hipi::rtc_probePcf85063(b, 0x51, name, detail) == hipi::I2cVerdict::Fault && name == "PCF85063A" &&
              detail == "registers garbled -- set the time", "probe: PCF85063A with garbled registers -> fault");
    }
    // An EEPROM on 0x51: not a clock, nothing written
    chips.clear();
    addChip(0x51, Kind::EEPROM);
    {
        CRtc rtc(&bus0);
        std::string name, detail;
        Bus b;
        const std::uint8_t before = chips[0x51].regs[0x00];
        check(!rtc.begin(16, 17) && hipi::rtc_probePcf85063(b, 0x51, name, detail) == hipi::I2cVerdict::NotIt &&
              chips[0x51].regs[0x00] == before, "EEPROM on 0x51: not a PCF85063A, nothing written");
    }
    // Not taken: its registers kept for the log
    chips.clear();
    addChip(0x51, Kind::PCF8563);
    {
        CRtc rtc(&bus0);
        std::uint8_t r[11];
        check(!rtc.begin(16, 17) && rtc.answeredOnPcfAddress(r) && r[0x03] == 0x47 && r[0x0A] == chips[0x51].regs[0x0A],
              "0x51 answers, not a PCF85063A: its registers kept for the log");
    }

    // ── 0x68 first, then 0x51 ──────────────────────────────────────────
    chips.clear();
    addChip(0x68, Kind::DS3231);
    addChip(0x51, Kind::PCF85063A);
    {
        CRtc rtc(&bus0);
        check(rtc.begin(16, 17) && rtc.chip() == RtcChip::DS3231 && rtc.address() == 0x68,
              "DS3231 and PCF85063A: the DS3231 is used");
        check(rtc.set(when(2026, 10, 17, 8, 0, 0)) && rtc.valid(), "DS3231: set, valid");
        check(chips[0x51].regs[0x04] == 0x80, "DS3231 and PCF85063A: the PCF85063A untouched");
    }
    chips.clear();
    addChip(0x68, Kind::DS1307);
    {
        CRtc rtc(&bus0);
        check(rtc.begin(16, 17) && rtc.chip() == RtcChip::DS1307, "DS1307: still told from the DS3231");
    }

    // ── Not a PCF85063A: a PCF8563 on 0x51, nothing at all ─────────────
    chips.clear();
    addChip(0x51, Kind::PCF8563);
    {
        CRtc rtc(&bus0);
        check(!rtc.begin(16, 17) && !rtc.present(), "PCF8563 on 0x51: not taken for a PCF85063A");
        check(chips[0x51].regs[0x03] == 0x47, "PCF8563 on 0x51: nothing written");
    }
    chips.clear();
    {
        CRtc rtc(&bus0);
        check(!rtc.begin(16, 17) && rtc.chip() == RtcChip::None, "no chip: not found");
        addChip(0x51, Kind::PCF85063A);
        check(rtc.begin(16, 17) && rtc.chip() == RtcChip::PCF85063A, "plugged in later: found");
    }

    // ── The Hardware test page's checks (rtc_probe.cpp) ────────────────
    Bus bus;
    chips.clear();
    addChip(0x51, Kind::PCF85063A);
    addChip(0x68, Kind::DS3231);
    timeUs = 10'000'000;
    {
        std::string name, detail;
        check(hipi::rtc_probePcf85063(bus, 0x51, name, detail) == hipi::I2cVerdict::Fault &&
              name == "PCF85063A" && detail == "00:00:00  time lost", "probe: PCF85063A after power-up -> fault, time lost");
        std::uint8_t set[] = { 0x04, 0x30, 0x47, 0x19, 0x17, 0x06, 0x10, 0x26 };
        fakeI2cWrite(0x51, set, sizeof(set));
        timeUs += 2'000'000;
        chips[0x51].regs[0x04] = 0x32;       // it ticked
        check(hipi::rtc_probePcf85063(bus, 0x51, name, detail) == hipi::I2cVerdict::Ok && detail == "19:47:32",
              "probe: PCF85063A set and ticking -> OK");
        timeUs += 2'000'000;                 // ... and then it didn't
        check(hipi::rtc_probePcf85063(bus, 0x51, name, detail) == hipi::I2cVerdict::Fault &&
              detail == "19:47:32  stopped", "probe: PCF85063A not ticking -> stopped");
        check(hipi::rtc_probeDs(bus, 0x68, name, detail) == hipi::I2cVerdict::Fault && name == "DS3231" &&
              detail == "00:00:00  24.25 C  time lost", "probe: DS3231 as before");
    }
    chips.clear();
    addChip(0x51, Kind::PCF8563);
    {
        std::string name, detail;
        check(hipi::rtc_probePcf85063(bus, 0x51, name, detail) == hipi::I2cVerdict::NotIt,
              "probe: a PCF8563 isn't a PCF85063A");
    }

    // ── The built-in chip list (sdcard/i2c/chips.txt) ──────────────────
    std::ifstream f("sdcard/i2c/chips.txt");
    std::stringstream ss;
    ss << f.rdbuf();
    hipi::I2cChipList list;
    list.parse(ss.str(), "chips.txt");
    list.finish(7);
    check(!ss.str().empty() && list.errors().empty(), "chips.txt: read without errors");
    for (const std::string& e : list.errors()) std::printf("     %s\n", e.c_str());
    const std::map<std::string, hipi::I2cBuiltin> probes = {
        { "clock", hipi::rtc_probeDs }, { "pcf85063", hipi::rtc_probePcf85063 } };

    chips.clear();
    addChip(0x51, Kind::PCF85063A);
    {
        std::uint8_t set[] = { 0x04, 0x30, 0x47, 0x19, 0x17, 0x06, 0x10, 0x26 };
        fakeI2cWrite(0x51, set, sizeof(set));
        timeUs += 5'000'000;
        hipi::I2cFound found;
        found.addr = 0x51;
        const hipi::I2cChip* which = nullptr;
        check(list.identify(bus, 0x51, found, probes, &which) && std::strcmp(found.name, "PCF85063A") == 0 &&
              found.state == hipi::I2cState::Ok, "chips.txt: 0x51 is the PCF85063A (not an EEPROM)");
        std::printf("     %s  %s  %s\n", found.name, found.what, found.detail);
        if (which != nullptr) {
            const std::map<std::string, hipi::I2cMeasureBuiltin> none;
            struct { const char* m; const char* want; } ms[] = {
                { "TIME", "19.4730" }, { "DATE", "2026.1017" }, { "DMY", "17.102026" }, { "MDY", "10.172026" } };
            for (const auto& m : ms) {
                std::string out, err;
                const hipi::I2cMeasure* meas = which->measure(m.m);
                const bool ok = meas != nullptr && hipi::I2cChipList::measure(bus, 0x51, *meas, -1, out, err, none) &&
                                out == m.want;
                char what[64];
                std::snprintf(what, sizeof(what), "TFI2C M%s -> %s (got %s)", m.m, m.want, out.c_str());
                check(ok, what);
            }
        }
    }
    chips.clear();
    addChip(0x51, Kind::PCF8563);
    {
        hipi::I2cFound found;
        found.addr = 0x51;
        list.identify(bus, 0x51, found, probes);
        check(std::strcmp(found.name, "PCF85063A") != 0, "chips.txt: a PCF8563 on 0x51 isn't called PCF85063A");
    }

    std::puts(failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
