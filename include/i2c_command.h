// i2c_command.h -- TFI2C's commands (ili2c.h): a line from the controller
// in, the answer out. No hardware here (I2cBus), so it can be tested on
// the PC (tools/i2ctest).
//
// A line holds one or more commands separated by ';' -- or by ',' when a
// letter follows (A118,MT: easier on the HP-41). Numbers are decimal,
// or hex with a '$' in front ($68); answers are always decimal. The answer
// of a line is that of its last command that gives one.
//
//   A<addr>        chip address (stays)        A104   A$68   ABMP280 (by name)
//   R<reg>[,<n>]   read n bytes (default 1)    R$0D  -> 171     R5,2 -> 17,168
//   R<reg>:<type>  read a number               R5:S16 -> -240   (U8 S8 U16 S16 U16L S16L)
//   W<reg>,<b>...  write a register             W$6B,0
//   W,<b>,<b>...   write bytes (a command)      W,172,51,0
//   Q<n>           read n bytes, no register    Q7 -> 28,115,...
//   D<ms>          wait (at most 2000 ms)       D80
//   M<name>        a measurement from i2c/*.txt MT  MTEMP  M TEMP -> 23.4
//                  (no chip chosen: the first chip that has it)
//   L              the chip's measurements      L -> T,P
//   P<n>           decimals for M (P: the file's)  P2
//   S              scan the bus                 S -> 56,104,118
//   I              what the chip is             I -> DS3231 OK
//
// Errors answer "ERR <what>": NOACK (the chip didn't answer), CHIP (no
// address), HIPI (a chip HIPI itself uses: writing refused), NAME
// (unknown chip or measurement), CRC, TIME (waits over 2000 ms), RANGE,
// DATA, SYNTAX.
#pragma once
#include "i2c_chips.h"
#include <functional>
#include <map>
#include <set>
#include <string>

namespace hipi {

struct I2cCommandEnv {
    I2cBus* bus = nullptr;
    std::function<const I2cChipList&()> chips;                                 // the descriptions
    std::function<int(I2cFound* out, int max, bool found[128])> identifyAll;   // scan + identify (i2c_identify())
    std::map<std::string, I2cBuiltin> probes;                                  // "probe <name>"
    std::map<std::string, I2cMeasureBuiltin> measures;                         // "builtin <name>"
    std::function<bool(std::uint8_t addr)> hipiOwns;                           // writes refused there
};

class I2cCommander {
public:
    explicit I2cCommander(I2cCommandEnv env) : env_(std::move(env)) {}

    // Runs a line; returns the answer ("" = none)
    std::string run(const std::string& line);
    // True if the line only reads (it may then be run again for each
    // reading, a fresh value every time)
    static bool readOnly(const std::string& line);
    // True if the line only sets things (A: the chip, P: decimals) -- it's
    // run at once, so "A$29" OUTA then "MD" OUTA works as one line would
    static bool settingsOnly(const std::string& line);
    // DCL: no chip chosen, decimals from the file, setups done again
    void reset();
    std::uint8_t address() const { return addr_; }

private:
    bool command(const std::string& cmd, std::string& answer);   // false: stop (answer = ERR ...)
    const I2cChip* chipAt(std::uint8_t addr, std::string* name = nullptr);
    bool doMeasure(const std::string& name, std::string& answer);

    I2cCommandEnv env_;
    std::uint8_t addr_ = 0;                 // 0 = none chosen
    int dec_ = -1;                          // P: decimals (-1 = the file's)
    std::set<std::uint8_t> setupDone_;
    int waited_ = 0;                        // D in this line (ms)
};

}  // namespace hipi
