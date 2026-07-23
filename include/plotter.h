#ifndef __PLOTTER_H__
#define __PLOTTER_H__

// CPlotter -- a minimal HP-7475A-compatible HP-IL plotter device.
//
// Unlike pyILPER's cls_pilplotter (see pilplotter.py), which forwards raw
// HP-GL text to an external "emu7470" subprocess and receives back
// structured drawing commands, this class implements the HP-GL parsing and
// plotter state machine itself -- there's no external interpreter to lean
// on in this environment. The tokenizer below (process_char()-equivalent
// logic in feed_char()) is ported from pyILPER's cls_HP7470Processor,
// simplified for the command set actually supported here.
//
// v1 scope (by design, not yet a full HP-GL/2 implementation):
//   IN  - initialize: clears the plot and resets pen state
//   SP  - select pen (just tracks a pen index/color, no physical carousel)
//   PU  - pen up, optionally followed by one or more coordinate pairs
//         (moves without drawing)
//   PD  - pen down, optionally followed by one or more coordinate pairs
//         (moves *and* draws a line per pair)
//   PA  - switch to absolute coordinates, then move/draw through any
//         coordinate pairs given (using whichever pen state is current)
//   PR  - switch to relative coordinates (deltas from the current pen
//         position), then move/draw the same way
//   OP  - output P1/P2 (the plotter's hard-clip scaling points) -- required
//         for the HP-41's PINIT ROM routine to complete its handshake;
//         responds with the real 7475A factory defaults (250,279 /
//         10250,7479), since we don't yet support IP/user-scaling that
//         would change them
//   OA  - output actual pen position + up/down status
//   OC  - output commanded pen position + up/down status (same format as
//         OA -- we don't simulate motor lag, so these never differ here)
//   OE  - output error status (always reports "no error" -- v1 doesn't
//         track real error conditions yet)
//
// Not yet supported: SC/IP user-scaling, text labels (LB), digitizing,
// other output/status queries (OS, OD, OF, OI, OO, OW) -- left for a
// later pass once the display-rendering side (step two) exists to drive
// priorities from.
//
// Output is exposed two ways, for step two (actual rendering) to use:
//   - Live callbacks (onMove/onDraw/onPenChange/onClear), fired as each
//     HP-GL command is executed.
//   - segments(): the full list of drawn line segments so far, mirroring
//     Screen::lines_ -- lets a later redraw reconstruct the whole plot
//     (e.g. after a menu has been drawn over the plot area) without
//     replaying the original HP-IL byte stream.

#include "hpil.h"
#include "usb_serial.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Same global trace flag used throughout the project (set via the touch
// UI's Config > Trace menu) -- gates the per-command/per-move logging
// below so it's silent unless you've actually asked for trace output.
extern bool bTrace;

struct PlotSegment {
    std::int16_t x0, y0, x1, y1;
    std::uint8_t pen;
};

class CPlotter : public CDevice {
public:
    // sai/aau/device-id match the real HP-7475A/pyILPER conventions:
    // AID 0x60, default address 5, device-id string "HP7470A".
    CPlotter(const char *name, IL_ADDR_t _sai = 0x60, IL_ADDR_t _aau = 5)
        : CDevice(name, _sai, _aau, PLOTTER) {
        reset_state();
    }

    void clear(void);   // HP-IL "Clear Device" (DCL/SDC) -- resets everything
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn);
    void show(void);    // extended-trace detail: pen/position/mode/parser state

    // Registered by whatever drives the actual display (step two).
    void setMoveCallback(std::function<void(std::int16_t, std::int16_t)> cb) {
        onMove_ = std::move(cb);
    }
    void setDrawCallback(std::function<void(std::int16_t, std::int16_t,
                                             std::int16_t, std::int16_t,
                                             std::uint8_t)> cb) {
        onDraw_ = std::move(cb);
    }
    void setPenChangedCallback(std::function<void(std::uint8_t, bool)> cb) {
        onPenChanged_ = std::move(cb);
    }
    void setClearCallback(std::function<void()> cb) {
        onClear_ = std::move(cb);
    }

    const std::vector<PlotSegment>& segments() const { return segments_; }
    std::int16_t penX() const { return penX_; }
    std::int16_t penY() const { return penY_; }
    bool penDown() const { return penDown_; }
    std::uint8_t currentPen() const { return currentPen_; }

private:
    enum class CoordMode { Absolute, Relative };

    void reset_state();
    void feed_char(char c);
    void execute(const std::string& cmd);
    std::vector<double> parseParams(const std::string& cmd, std::size_t from) const;
    void moveThroughParams(const std::vector<double>& params);
    void doMove(std::int16_t x, std::int16_t y);
    void queueOutput(const std::string& s);

    // ── Tokenizer state (ported from pyILPER's process_char()) ──────────
    // 0: waiting for the command's first letter
    // 1: waiting for the second letter
    // 2: collecting numeric parameters until a non-parameter character
    // 3: inside LB label text, discarding verbatim until the terminator
    //    (default ETX/0x03) -- see feed_char() in plotter.cpp
    int parseState_ = 0;
    std::string cmdBuf_;
    bool inParam_ = false;
    bool sawSeparator_ = false;
    int numParam_ = 0;

    // ── Plotter state ─────────────────────────────────────────────────
    std::int16_t penX_ = 0, penY_ = 0;
    bool penDown_ = false;
    std::uint8_t currentPen_ = 1;
    CoordMode coordMode_ = CoordMode::Absolute;

    std::vector<PlotSegment> segments_;
    std::function<void(std::int16_t, std::int16_t)> onMove_;
    std::function<void(std::int16_t, std::int16_t, std::int16_t, std::int16_t, std::uint8_t)> onDraw_;
    std::function<void(std::uint8_t, bool)> onPenChanged_;
    std::function<void()> onClear_;

    // ── Talker (query response) output queue ─────────────────────────
    // Populated by OP/OA/OE (see execute()) and drained one byte per
    // incoming SDA frame while this device is the addressed talker --
    // mirrors CDrive's doTalker()/doNextTalker() convention (SDA = send
    // next byte, NRD = no more data expected, ETO = end-of-transfer
    // response once the queue is empty).
    std::vector<std::uint8_t> outQueue_;
    bool outEnd_ = true;
    // True only while we're actively mid-transfer (an SDA has already
    // started one) -- lets doTalker() safely recognize the echoed data
    // byte looping back around the physical bus as "continue", without
    // ever mistaking routine bus traffic (RFC, AAU, addressing, IFC, ...)
    // for that, since those never satisfy IS_DATA() anyway.
    bool midTransfer_ = false;
};

#endif//__PLOTTER_H__