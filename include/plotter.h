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
// The IW clip-window and LB text-label support are ported from Mike's
// oo7470A.rx (an ooRexx HP7470A/HP-IL simulator, freeware under the Q
// Public License) -- specifically its drawline:/plolet: routines (line
// clipping) and its chagra. stroke-font table (see include/hpgl_font.h).
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
//   TL  - tick length (percentage of the P1-P2 span, positive/negative
//         direction), used by XT/YT below
//   XT  - draw a small tick mark through the current pen position,
//         perpendicular to the X axis (i.e. a short vertical stroke),
//         without moving the pen
//   YT  - same as XT but perpendicular to the Y axis (a short horizontal
//         stroke)
//   IW  - input window: a clip rectangle (defaults to P1-P2) applied to
//         every drawn line (PD segments, ticks, and label strokes) --
//         anything outside gets clipped or dropped entirely, matching the
//         real plotter's hard-clip behavior
//   SR  - character size, as a percentage of the P1-P2 span (width,height)
//   SL  - character slant (a shear factor applied to each glyph's x)
//   DI  - label direction, as a (run,rise) vector -- rotates subsequent
//         LB text (and CP's positioning) to match
//   CP  - character plot: repositions the "text cursor" by a number of
//         character widths/heights (from SR), along the current DI
//         direction, relative to the current pen position
//   LB  - label: draws text using a stroke ("stick") font (see
//         include/hpgl_font.h) terminated by ETX (0x03) -- DT (redefining
//         the terminator) isn't supported, so it's always ETX
//
// Not yet supported: SC/IP user-scaling, digitizing, other output/status
// queries (OS, OD, OF, OI, OO, OW), DT -- left for a later pass.
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

    // Draws a short tick mark through the current pen position without
    // moving it -- vertical=true for XT (perpendicular to the X axis),
    // false for YT. Length comes from tickLenPos_/tickLenNeg_ (set by TL,
    // as a percentage of the P1-P2 span), regardless of the current
    // PU/PD pen state (a tick always draws, using the current pen color).
    void drawTick(bool vertical);

    // Single choke point for every drawn segment (PD lines, ticks, and
    // label strokes) -- clips to the current IW window (clipToWindow())
    // before pushing to segments_/firing onDraw_, dropping the segment
    // entirely if it's fully outside.
    void emitSegment(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1);

    // Cohen-Sutherland line clipping against [iw1x_,iw1y_]-[iw2x_,iw2y_].
    // Returns false if the segment is entirely outside (nothing to draw);
    // otherwise x0/y0/x1/y1 are adjusted in place to the clipped endpoints.
    bool clipToWindow(double& x0, double& y0, double& x1, double& y1) const;

    // Renders one LB label's text, character by character.
    void drawLabel(const std::string& text);

    // Renders a single character's stroke glyph (see include/hpgl_font.h)
    // at the current character position (charPosX_/charPosY_), scaled by
    // charWidthUnits_/charHeightUnits_ (SR), sheared by charSlant_ (SL),
    // and rotated by dirCos_/dirSin_ (DI) -- then advances the character
    // position to the next character's origin.
    void drawLabelChar(unsigned char code);

    // ── Tokenizer state (ported from pyILPER's process_char()) ──────────
    // 0: waiting for the command's first letter
    // 1: waiting for the second letter
    // 2: collecting numeric parameters until a non-parameter character
    // 3: inside LB label text, accumulating verbatim until the terminator
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

    // Tick length as a percentage of the P1-P2 span, in the positive and
    // negative directions -- set by TL, used by XT/YT. HP-GL default: 0.5
    // for both.
    double tickLenPos_ = 0.5, tickLenNeg_ = 0.5;

    // Input window (clip rectangle) -- set by IW, defaults to P1-P2.
    std::int16_t iw1x_ = 0, iw1y_ = 0, iw2x_ = 0, iw2y_ = 0;

    // ── Label (LB) text state ────────────────────────────────────────
    // Character size in plotter units (SR, as a percentage of the P1-P2
    // span -- HP-GL defaults to 0.75%/1.5% width/height).
    double charWidthUnits_ = 0.0, charHeightUnits_ = 0.0;
    double charSlant_ = 0.0;                  // SL -- shear factor, default 0
    double dirCos_ = 1.0, dirSin_ = 0.0;       // DI -- default: horizontal
    // "Text cursor" position -- kept in sync with penX_/penY_ by doMove()
    // on every real pen movement, but overridden by CP to offset it by a
    // number of character cells without touching the actual pen position.
    std::int16_t charPosX_ = 0, charPosY_ = 0;

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

// The single CPlotter instance created in hipi_init() (see hipi.cpp) --
// exposed so other modules (e.g. plotterview.cpp) can wire up its
// callbacks/segments() without needing to search the `devices` vector.
// Matches the same pattern as pilbox.h's `extern CPilBox* pilbox;`.
extern CPlotter* plotter;

#endif//__PLOTTER_H__
