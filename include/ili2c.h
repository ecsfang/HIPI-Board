#ifndef __ILI2C_H__
#define __ILI2C_H__

// CHipiI2c -- TFI2C: the I2C bus for the controller. Instead of a driver
// per chip, the controller reads and writes the chips'
// registers itself, or asks for a measurement described in i2c/*.txt on
// the SD card (sdcard/i2c/chips.txt) -- "MT" gives e.g. 23.4. The commands
// are in i2c_command.h.
//
// Listener: the bytes of a line, ended by CR or LF (or UNL). A line that
// only reads (no W) is kept and run again for every reading, so each INA
// gives a fresh value; a line that writes runs at once.
// Talker: SDA sends the answer, then CR LF, then ETO.
// DCL/SDC: no chip chosen, the chip descriptions read again.
//
// Accessory ID 0x52 (electronic instrumentation, measurement).

#include "hpil.h"
#include "i2c_command.h"
#include <functional>
#include <string>
#include <vector>

class CHipiI2c : public CDevice {
public:
    CHipiI2c(const char* name, hipi::I2cCommandEnv env, std::function<void()> reload, IL_ADDR_t sai = 0x52,
             IL_ADDR_t aau = 31)
        : CDevice(name, sai, aau, NONE), commander_(std::move(env)), reload_(std::move(reload)) {}

    IL_CMD_t hpil(IL_CMD_t cmd) override;
    void doListener(IL_CMD_t cmd, IL_CMD_t* rtn) override;
    void doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) override;
    void clear() override;
    void ifc() override { setIdle(); }

private:
    void endLine();

    hipi::I2cCommander commander_;
    std::function<void()> reload_;
    std::string inLine_;                 // the line coming in
    std::string repeat_;                 // a read-only line: run for every reading
    std::string answer_;                 // a writing line's answer
    std::vector<char> out_;              // the answer being sent
    bool midTransfer_ = false;
};

namespace hipi {
// TFI2C's bus, chip descriptions and built-in checks (i2c_ident.cpp)
I2cCommandEnv i2c_commandEnv();
}

#endif//__ILI2C_H__
