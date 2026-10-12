// i2c_chips.h -- the I2C chips HIPI knows, described in text files
// (i2c/*.txt on the SD card, or the built-in copy of sdcard/i2c/chips.txt)
// and the matching of a chip on the bus against them. No hardware here:
// the bus is reached through I2cBus, the built-in checks through
// I2cBuiltin, so this can be tested on the PC (tools/i2ctest).
//
// The file format (see sdcard/i2c/chips.txt for the full description):
//
//   chip 0x76-0x77 BMP280 | temperature/pressure
//     id 0xD0 0x58=BMP280 0x60=BME280      one byte at a register
//     id16 0x01 0x60A0 mask=0xFF00         two bytes (big-endian)
//     text 0x0A "H43"                      characters at a register
//     status 0x08 0x08                     one byte read without a register
//     show 0x00 s16 shift=4 scale=0.0625 unit=C dec=1   a reading
//     range -40 125                        ... that must be within this
//     probe clock                          a check built into the firmware
//     expected                             always there (missing if silent)
//     panel 7                              only on this panel
//     setup                                done once before the first measurement
//       write 0x6B 0x00
//     measure T,TEMP | temperature         a measurement TFI2C can make ("MT")
//       write 0xAC 0x33 0x00               bytes to the chip (the first is often a register)
//       wait 80                            milliseconds
//       read 7 crc=aht                     n bytes; "read <reg> <n>": from a register
//       value bytes=3-5 mask=0xFFFFF scale=0.00019073486 offset=-50 dec=1 unit=C
//       format bcd 2/0x3F 1 0 sep=:        ... or the bytes as BCD text (a time)
//       format bcd 2/0x3F text=. 1 0       ... with text between: 12.3456 (HP-41 HH.MMSS)
//       builtin bosch.T                    ... or a measurement built into the firmware
//
// A chip with checks is tried before one without (that's only "probably"
// that chip); among those, in the order they're read.
#pragma once
#include "i2c_ident.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hipi {

// Reads from the bus (the firmware: the I2C hardware; a test: made up)
struct I2cBus {
    virtual ~I2cBus() = default;
    // n bytes from register reg (the register number written first)
    virtual bool readRegs(std::uint8_t addr, std::uint8_t reg, std::uint8_t* buf, std::size_t n) = 0;
    // n bytes, no register
    virtual bool readRaw(std::uint8_t addr, std::uint8_t* buf, std::size_t n) = 0;
    // n bytes to the chip (TFI2C and measurements only -- identifying never writes)
    virtual bool write(std::uint8_t addr, const std::uint8_t* buf, std::size_t n) = 0;
    virtual void delayMs(int ms) = 0;
};

// A measurement built into the firmware ("builtin <name>", e.g. bosch.T)
using I2cMeasureBuiltin = std::function<bool(I2cBus& bus, std::uint8_t addr, double& value)>;

// A check built into the firmware ("probe <name>"): says Ok / Fault / not
// this chip, may rename it and give a detail
enum class I2cVerdict : std::uint8_t { NotIt, Ok, Fault };
using I2cBuiltin = std::function<I2cVerdict(I2cBus& bus, std::uint8_t addr, std::string& name, std::string& detail)>;

struct I2cRule {
    enum class Kind : std::uint8_t { Id, Id16, Text, Status, Probe } kind = Kind::Id;
    std::uint8_t reg = 0;
    std::uint16_t mask = 0xFFFF;
    std::vector<std::pair<std::uint16_t, std::string>> values;   // value, name ("" = keep)
    std::string text;                                            // Text: the characters; Probe: its name
};

// A number from bytes: a chip's "show" reading, a measurement's "value"
struct I2cShow {
    bool on = false;
    std::uint8_t reg = 0;
    enum class Type : std::uint8_t { U8, S8, U16, S16, U16LE, S16LE } type = Type::U8;
    int b0 = 0, b1 = -1;          // value: bytes=b0-b1 (big-endian, unsigned), b1 < 0: by the type
    bool le = false;              // ... little-endian
    double offset = 0;
    std::uint32_t mask = 0xFFFFFFFF;
    int bits = 0;                 // sign bit position + 1 after masking (0 = as the type)
    int shift = 0;                // right shift after masking
    double scale = 1.0;
    std::string unit;
    int dec = 0;                  // decimals shown
    bool hasRange = false;
    double min = 0, max = 0;
};

// A step of a measurement (or setup)
struct I2cStep {
    enum class Kind : std::uint8_t { Write, Wait, Read, ReadReg } kind = Kind::Write;
    std::vector<std::uint8_t> bytes;   // Write
    std::uint8_t reg = 0;              // ReadReg
    int n = 0;                         // Read/ReadReg: bytes; Wait: ms
    enum class Crc : std::uint8_t { None, Sensirion, Aht, Htu } crc = Crc::None;
};

struct I2cMeasure {
    std::vector<std::string> names;    // T, TEMP (upper case)
    std::string what;
    std::vector<I2cStep> steps;
    enum class Out : std::uint8_t { Value, Bcd, Builtin } out = Out::Value;
    I2cShow value;
    std::vector<std::pair<int, std::uint8_t>> bcd;   // byte, mask (byte -1: the next of texts)
    std::vector<std::string> texts;                  // text=.. pieces, in order
    std::string sep = ":";                           // between two bytes ("" when there are texts)
    std::string builtin;
};

struct I2cChip {
    std::uint8_t first = 0, last = 0;
    std::string name, what;
    std::vector<I2cRule> rules;
    I2cShow show;
    bool expected = false;
    int panel = 0;                // 0 = both, 5 or 7
    std::vector<I2cStep> setup;
    std::vector<I2cMeasure> measures;
    const I2cMeasure* measure(const std::string& name) const;   // by any of its names (any case)
    std::string source;           // "chips.txt:12" (for messages)
    bool checks() const { return !rules.empty() || show.on; }
};

class I2cChipList {
public:
    // Adds the chips described in `text` (source: a name for messages).
    // Lines it can't read are skipped and noted in errors().
    void parse(const std::string& text, const std::string& source);
    // Chips with checks first, in reading order, then the others
    void finish(int panel);
    const std::vector<I2cChip>& chips() const { return chips_; }
    const std::vector<std::string>& errors() const { return errors_; }
    void clear() { chips_.clear(); errors_.clear(); }

    // What answers on addr: fills f (name, what, state, detail). False if
    // no chip in the list fits (then it's unknown).
    bool identify(I2cBus& bus, std::uint8_t addr, I2cFound& f,
                  const std::map<std::string, I2cBuiltin>& builtins, const I2cChip** which = nullptr) const;
    // The chips that must be there, on addr (for "missing")
    const I2cChip* expectedAt(std::uint8_t addr) const;

    // Addresses where a register number must never be written (a PCF8574
    // there would take it as its output levels): only "status" checks
    static bool noRegisterWrites(std::uint8_t addr) { return addr >= 0x20 && addr <= 0x27; }

    // Runs the steps (setup or a measurement): the bytes read (CRC bytes
    // checked and dropped). False with err set: "NOACK", "CRC", "TIME".
    static bool run(I2cBus& bus, std::uint8_t addr, const std::vector<I2cStep>& steps,
                    std::vector<std::uint8_t>& data, std::string& err);
    // A measurement: the answer as text (decimals: dec < 0 = the file's).
    // False with err set (also "NAME" for an unknown builtin).
    static bool measure(I2cBus& bus, std::uint8_t addr, const I2cMeasure& m, int dec, std::string& out,
                        std::string& err, const std::map<std::string, I2cMeasureBuiltin>& builtins,
                        double* value = nullptr);
    // A number from bytes (see I2cShow); false if there are too few bytes
    static bool decode(const I2cShow& s, const std::uint8_t* buf, std::size_t n, double& value);
    static constexpr int kMaxWaitMs = 2000;    // waits in one measurement, at most

private:
    std::vector<I2cChip> chips_;
    std::vector<std::string> errors_;
};

}  // namespace hipi
