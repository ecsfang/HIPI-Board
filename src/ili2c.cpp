// ili2c.cpp -- see ili2c.h
#define MODULE "I2C"
#include "ili2c.h"
#include "usb_serial.h"

IL_CMD_t CHipiI2c::hpil(IL_CMD_t cmd) {
    // OUTA without CR LF (flag 17): the line ends with UNL
    if (cmd == UNL && isListener() && !inLine_.empty()) endLine();
    return CDevice::hpil(cmd);
}

void CHipiI2c::endLine() {
    const std::string line = inLine_;
    inLine_.clear();
    if (hipi::I2cCommander::settingsOnly(line)) {
        // Only settings (A, P): done now, so a following line ("MD") uses them
        repeat_.clear();
        answer_ = commander_.run(line);           // (empty, or an error: ERR NAME ...)
        MTRC_LOGF("line \"%s\" (settings) -> \"%s\"", line.c_str(), answer_.c_str());
    } else if (hipi::I2cCommander::readOnly(line)) {
        repeat_ = line;                          // run for each reading (SDA)
        answer_.clear();
        MTRC_LOGF("line \"%s\" (run at each reading)", line.c_str());
    } else {
        repeat_.clear();
        answer_ = commander_.run(line);           // writes: at once
        MTRC_LOGF("line \"%s\" -> \"%s\"", line.c_str(), answer_.c_str());
    }
}

void CHipiI2c::doListener(IL_CMD_t cmd, IL_CMD_t* rtn) {
    *rtn = cmd;
    if (!IS_DATA(cmd)) return;
    const char c = static_cast<char>(cmd & 0xFF);
    if (c == '\r' || c == '\n') {
        if (!inLine_.empty()) endLine();
        return;
    }
    if (inLine_.size() < 200) inLine_ += c;
}

void CHipiI2c::doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) {
    if (cmd == NRD) {                            // no more wanted
        out_.clear();
        return;
    }
    if (cmd != SDA && !(midTransfer_ && IS_DATA(cmd))) return;
    if (cmd == SDA && !midTransfer_) {
        if (!inLine_.empty()) endLine();         // (no CR LF, no UNL yet)
        std::string a = repeat_.empty() ? answer_ : commander_.run(repeat_);
        if (!repeat_.empty()) MTRC_LOGF("\"%s\" -> \"%s\"", repeat_.c_str(), a.c_str());
        out_.assign(a.begin(), a.end());
        if (!a.empty()) { out_.push_back('\r'); out_.push_back('\n'); }
    }
    if (out_.empty()) {
        *rtn = ETO;
        midTransfer_ = false;
    } else {
        *rtn = static_cast<IL_CMD_t>(static_cast<std::uint8_t>(out_.front()));
        out_.erase(out_.begin());
        midTransfer_ = true;
    }
}

void CHipiI2c::clear() {
    MTRC_LOGF("clear (DCL/SDC): the chip descriptions read again");
    inLine_.clear();
    repeat_.clear();
    answer_.clear();
    out_.clear();
    midTransfer_ = false;
    commander_.reset();
    if (reload_) reload_();
}
