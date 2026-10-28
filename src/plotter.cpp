// plotter.cpp -- CPlotter (plotter.h): the HP 7470A's HP-IL side. The
// HP-GL itself is HpglInterpreter's (hpgl.cpp).
#define MODULE "PLOTTER"

#include "plotter.h"
#include "usb_serial.h"

extern bool bTrace;

CPlotter::CPlotter(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau)
    : CDevice(name, _sai, _aau, PLOTTER), hpgl_(name, kHp7470A) {
    // An O.. instruction's answer: sent as talker (doTalker())
    hpgl_.setOutputCallback([this](const std::string& s) {
        outQueue_.assign(s.begin(), s.end());
        outEnd_ = false;
        MTRC_LOGF("queued response: \"%s\"", s.c_str());
    });
    // IN (or a device clear): nothing left to send
    hpgl_.setResetCallback([this] { resetTalker(); });
}

void CPlotter::resetTalker() {
    outQueue_.clear();
    outEnd_ = true;
    midTransfer_ = false;
    sstPending_ = false;
}

void CPlotter::clear(void) {
    MTRC_LOGF("clear (DCL/SDC)");
    hpgl_.reset();
}

void CPlotter::doListener(IL_CMD_t cmd, IL_CMD_t *rtn) {
    *rtn = cmd;
    if (IS_DATA(cmd)) {
        hpgl_.put(static_cast<char>(cmd & 0xFF));
    }
}

// Drains outQueue_ one byte per incoming poll -- mirrors CDrive's
// doTalker()/doNextTalker() convention for NRD/ETO, but (per real hardware
// behavior) advances on *any* incoming frame while we're the addressed
// talker, not just a literal SDA. On the physical HP-IL loop, the byte we
// just sent circulates all the way around and arrives back as the next
// incoming frame -- so after the initial SDA trigger, every subsequent
// poll shows up as that echoed byte value, not another SDA. Treating only
// SDA as "send the next byte" left every later poll unhandled, so the
// first byte just kept getting echoed back forever instead of advancing.
void CPlotter::doTalker(IL_CMD_t cmd, IL_CMD_t *rtn) {
    // Send Status (manual, chapter 11, T2): the byte OS gives -- without
    // clearing "initialized" -- then ETO when it has come back round
    if (cmd == SST) {
        *rtn = static_cast<IL_CMD_t>(hpgl_.statusByte());
        sstPending_ = true;
        MTRC_LOGF("doTalker: SST -> %d", *rtn);
        return;
    }
    if (sstPending_ && (IS_DATA(cmd) || cmd == NRD)) {
        if (cmd == NRD) return;              // (ETO when our byte is back)
        *rtn = ETO;
        sstPending_ = false;
        return;
    }
    if (cmd == NRD) {
        // Do NOT clear midTransfer_ here -- the controller's own echoed
        // copy of the last byte we sent is still coming back around the
        // physical bus right after this, and it must still be recognized
        // as transfer-related (via midTransfer_ && IS_DATA(cmd) below) so
        // it gets turned into ETO. Clearing midTransfer_ here would leave
        // that final echoed byte completely unhandled instead.
        outEnd_ = true;
        MTRC_LOGF("doTalker: NRD -> end=true");
        return;
    }

    // Only actually participate in a data transfer if this is genuinely a
    // transfer-related trigger: the explicit SDA request, or -- while a
    // transfer is already under way -- the byte we just sent looping back
    // around the physical bus (always a plain DAB-class value). Anything
    // else (RFC, AAU, addressing frames, IFC, ...) is routine bus traffic
    // completely unrelated to us and must be left alone: hijacking it
    // (as an earlier version of this function did) breaks the
    // controller's normal bus-scan/keepalive cycle, making the whole
    // device disappear from the bus.
    if (cmd != SDA && !(midTransfer_ && IS_DATA(cmd))) {
        return;
    }

    if (outQueue_.empty()) {
        *rtn = ETO;
        midTransfer_ = false;
        MTRC_LOGF("doTalker: cmd=0x%03X -> ETO (nothing queued)", cmd);
    } else {
        *rtn = static_cast<IL_CMD_t>(outQueue_.front());
        outQueue_.erase(outQueue_.begin());
        midTransfer_ = true;
        if (bTrace) {
            MLOGF("doTalker: cmd=0x%03X -> 0x%02X '%c' (queue left=%zu)",
                 cmd, *rtn, (*rtn >= 32 && *rtn < 127) ? static_cast<char>(*rtn) : '.',
                 outQueue_.size());
        }
        if (outQueue_.empty()) outEnd_ = true;
    }
}

void CPlotter::show(void) {
    CDevice::show();
    hpgl_.show();
    LOGF("\r\n\toutQueue:%zu%s", outQueue_.size(), outEnd_ ? " (end)" : "");
}
