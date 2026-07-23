#include "plotter.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

void CPlotter::reset_state() {
    parseState_ = 0;
    cmdBuf_.clear();
    inParam_ = false;
    sawSeparator_ = false;

    penX_ = penY_ = 0;
    penDown_ = false;
    currentPen_ = 1;
    coordMode_ = CoordMode::Absolute;

    segments_.clear();

    outQueue_.clear();
    outEnd_ = true;
    midTransfer_ = false;
}

void CPlotter::clear(void) {
    if (bTrace) LOGF("\r\n[PLOTTER] clear (DCL/SDC)");
    reset_state();
    if (onClear_) onClear_();
}

void CPlotter::doListener(IL_CMD_t cmd, IL_CMD_t *rtn) {
    *rtn = cmd;
    if (IS_DATA(cmd)) {
        feed_char(static_cast<char>(cmd & 0xFF));
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
    if (cmd == NRD) {
        // Do NOT clear midTransfer_ here -- the controller's own echoed
        // copy of the last byte we sent is still coming back around the
        // physical bus right after this, and it must still be recognized
        // as transfer-related (via midTransfer_ && IS_DATA(cmd) below) so
        // it gets turned into ETO. Clearing midTransfer_ here would leave
        // that final echoed byte completely unhandled instead.
        outEnd_ = true;
        if (bTrace) LOGF("\r\n[PLOTTER] doTalker: NRD -> end=true");
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
        if (bTrace) LOGF("\r\n[PLOTTER] doTalker: cmd=0x%03X -> ETO (nothing queued)", cmd);
    } else {
        *rtn = static_cast<IL_CMD_t>(outQueue_.front());
        outQueue_.erase(outQueue_.begin());
        midTransfer_ = true;
        if (bTrace) {
            LOGF("\r\n[PLOTTER] doTalker: cmd=0x%03X -> 0x%02X '%c' (queue left=%zu)",
                 cmd, *rtn, (*rtn >= 32 && *rtn < 127) ? static_cast<char>(*rtn) : '.',
                 outQueue_.size());
        }
        if (outQueue_.empty()) outEnd_ = true;
    }
}

void CPlotter::queueOutput(const std::string& s) {
    outQueue_.assign(s.begin(), s.end());
    outEnd_ = false;
    if (bTrace) LOGF("\r\n[PLOTTER] queued response: \"%s\"", s.c_str());
}

// Tokenizer, ported from pyILPER's cls_HP7470Processor.process_char() --
// simplified since our command set has no LB (text label) or single-char
// (DT/SM) special cases to worry about.
void CPlotter::feed_char(char c) {
    switch (parseState_) {
    case 0:
        // First letter of a new command.
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cmdBuf_.clear();
            cmdBuf_ += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            parseState_ = 1;
        }
        break;

    case 1:
        // Second letter -- skip blanks first, same as the Python original.
        if (c == ' ') return;
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cmdBuf_ += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            parseState_ = 2;
            inParam_ = false;
            sawSeparator_ = false;
            numParam_ = 0;
        } else {
            cmdBuf_.clear();
            parseState_ = 0;
        }
        break;

    case 2:
        // Parameters: digits, '.', '+', '-'; ',' is an explicit separator;
        // spaces are ignored: anything else ends the command. Mirrors
        // pyILPER's numparam/separator bookkeeping so space-separated
        // params ("PA100 200") get a comma inserted between them, while
        // explicitly comma-separated ones ("PA100,200") don't get a
        // second one.
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '+' || c == '-') {
            if (!inParam_) {
                inParam_ = true;
                if (!sawSeparator_) {
                    if (numParam_ > 0) cmdBuf_ += ',';
                    ++numParam_;
                }
            }
            cmdBuf_ += c;
            return;
        }
        if (c == ' ') {
            inParam_ = false;
            return;
        }
        if (c == ',') {
            sawSeparator_ = true;
            inParam_ = false;
            cmdBuf_ += c;
            return;
        }
        // Anything else ends this command's parameter portion. LB (label)
        // is a special case: what follows isn't HP-GL syntax at all, it's
        // raw label text that runs verbatim up to a terminator (default
        // ETX/0x03, since we don't support DT redefining it yet) -- it
        // must NOT be fed back through the normal tokenizer, or letters
        // inside the label (e.g. a label containing "SP" or "PU") would
        // get misparsed as real commands and could corrupt plotter state.
        if (cmdBuf_.size() >= 2 && cmdBuf_[0] == 'L' && cmdBuf_[1] == 'B') {
            execute(cmdBuf_);  // no-op (LB unsupported in v1), keeps logging consistent
            cmdBuf_.clear();
            if (c == 0x03) {
                parseState_ = 0;  // empty label, already terminated
            } else {
                parseState_ = 3;  // consume label text verbatim
            }
            return;
        }
        execute(cmdBuf_);
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cmdBuf_.clear();
            cmdBuf_ += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            parseState_ = 1;
        } else {
            cmdBuf_.clear();
            parseState_ = 0;
        }
        break;

    case 3:
        // Inside LB label text -- discard verbatim until the terminator.
        if (c == 0x03) {
            parseState_ = 0;
        }
        break;
    }
}

std::vector<double> CPlotter::parseParams(const std::string& cmd, std::size_t from) const {
    std::vector<double> params;
    std::size_t pos = from;
    while (pos < cmd.size()) {
        std::size_t comma = cmd.find(',', pos);
        const std::string token = cmd.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!token.empty()) {
            params.push_back(std::strtod(token.c_str(), nullptr));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return params;
}

void CPlotter::doMove(std::int16_t x, std::int16_t y) {
    if (penDown_) {
        segments_.push_back({penX_, penY_, x, y, currentPen_});
        if (bTrace) {
            LOGF("\r\n[PLOTTER]   draw (%d,%d) -> (%d,%d) pen=%u",
                 penX_, penY_, x, y, currentPen_);
        }
        if (onDraw_) onDraw_(penX_, penY_, x, y, currentPen_);
    } else {
        if (bTrace) LOGF("\r\n[PLOTTER]   move -> (%d,%d)", x, y);
        if (onMove_) onMove_(x, y);
    }
    penX_ = x;
    penY_ = y;
}

void CPlotter::moveThroughParams(const std::vector<double>& params) {
    // Coordinates come in (x,y) pairs -- an odd trailing value (malformed
    // input) is ignored.
    for (std::size_t i = 0; i + 1 < params.size(); i += 2) {
        const std::int16_t x = static_cast<std::int16_t>(
            coordMode_ == CoordMode::Absolute ? params[i] : penX_ + params[i]);
        const std::int16_t y = static_cast<std::int16_t>(
            coordMode_ == CoordMode::Absolute ? params[i + 1] : penY_ + params[i + 1]);
        doMove(x, y);
    }
}

void CPlotter::execute(const std::string& cmd) {
    if (cmd.size() < 2) return;
    const std::string mnemonic = cmd.substr(0, 2);
    const std::vector<double> params = parseParams(cmd, 2);

    if (mnemonic == "IN") {
        if (bTrace) LOGF("\r\n[PLOTTER] IN (initialize/reset)");
        reset_state();
        if (onClear_) onClear_();
    } else if (mnemonic == "SP") {
        currentPen_ = params.empty() ? 0 : static_cast<std::uint8_t>(params[0]);
        if (bTrace) LOGF("\r\n[PLOTTER] SP -> pen=%u", currentPen_);
        if (onPenChanged_) onPenChanged_(currentPen_, penDown_);
    } else if (mnemonic == "PU") {
        penDown_ = false;
        if (bTrace) {
            LOGF("\r\n[PLOTTER] PU (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        if (onPenChanged_) onPenChanged_(currentPen_, penDown_);
        moveThroughParams(params);
    } else if (mnemonic == "PD") {
        penDown_ = true;
        if (bTrace) {
            LOGF("\r\n[PLOTTER] PD (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        if (onPenChanged_) onPenChanged_(currentPen_, penDown_);
        moveThroughParams(params);
    } else if (mnemonic == "PA") {
        coordMode_ = CoordMode::Absolute;
        if (bTrace) {
            LOGF("\r\n[PLOTTER] PA absolute (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        moveThroughParams(params);
    } else if (mnemonic == "PR") {
        coordMode_ = CoordMode::Relative;
        if (bTrace) {
            LOGF("\r\n[PLOTTER] PR relative (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        moveThroughParams(params);
    } else if (mnemonic == "OP") {
        // Output P1/P2 -- the plotter's hard-clip scaling points. Real
        // 7475A factory defaults; we don't support IP/user-scaling yet,
        // so these never change. The HP-41's PINIT ROM routine requires
        // this response to complete its startup handshake.
        constexpr int kP1X = 250, kP1Y = 279, kP2X = 10250, kP2Y = 7479;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d\r\n", kP1X, kP1Y, kP2X, kP2Y);
        if (bTrace) LOGF("\r\n[PLOTTER] OP (output P1/P2)");
        queueOutput(buf);
    } else if (mnemonic == "OA") {
        // Output Actual position: x,y,pen-status (0=up, 1=down).
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d\r\n", penX_, penY_, penDown_ ? 1 : 0);
        if (bTrace) LOGF("\r\n[PLOTTER] OA (output actual position)");
        queueOutput(buf);
    } else if (mnemonic == "OC") {
        // Output Commanded position -- same x,y,pen format as OA. Real
        // plotters distinguish "commanded" (where the last PA/PR/PU/PD
        // told the pen to go) from "actual" (where the carriage/motors
        // have physically settled to, accounting for acceleration/lag).
        // We don't simulate motor lag, so for us these are the same value.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d\r\n", penX_, penY_, penDown_ ? 1 : 0);
        if (bTrace) LOGF("\r\n[PLOTTER] OC (output commanded position)");
        queueOutput(buf);
    } else if (mnemonic == "OE") {
        // Output Error -- we don't track real plotter error conditions
        // yet, so this always reports "no error".
        if (bTrace) LOGF("\r\n[PLOTTER] OE (output error status)");
        queueOutput("0\r\n");
    } else if (bTrace) {
        // v1 scope (see plotter.h) -- unrecognized mnemonics are otherwise
        // silently ignored, but worth seeing while testing against real
        // programs to know what's not implemented yet.
        LOGF("\r\n[PLOTTER] unrecognized command '%s'", cmd.c_str());
    }
}

void CPlotter::show(void) {
    CDevice::show();
    LOGF("\r\n\tpen:%u %s pos:(%d,%d) mode:%s segments:%zu parser:'%s' outQueue:%zu%s",
         currentPen_, penDown_ ? "DOWN" : "UP", penX_, penY_,
         coordMode_ == CoordMode::Absolute ? "ABS" : "REL",
         segments_.size(), cmdBuf_.c_str(), outQueue_.size(),
         outEnd_ ? " (end)" : "");
}