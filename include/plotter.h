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
//   OP  - output P1/P2 (the scaling points: IP, factory defaults 250,279 /
//         10250,7479 after IN) -- required for the HP-41's PINIT ROM
//         routine to complete its handshake
//   CI  - circle: radius[,chord angle] around the pen position (pen up to
//         the start, down for the circle, back to the centre afterwards)
//   AA  - arc absolute: centre x,y, angle[,chord] -- from the pen, drawn if
//         the pen is down; AR: the same with the centre relative to the pen
//   IP  - input P1/P2 (bare: the defaults; two numbers: P1, P2 moves along)
//   SC  - scale: user units xmin,xmax,ymin,ymax mapped onto P1..P2 for
//         PA/PR/PU/PD (rounded to plotter units); bare SC: plotter units
//   OA  - output actual pen position + up/down status
//   OC  - output commanded pen position + up/down status (same format as
//         OA -- we don't simulate motor lag, so these never differ here)
//   OE  - output error: 1 = instruction not recognized (sets OS bit 5;
//         reading OE clears both), 0 = none
//   OI  - output identification (always "7470A")
//   OF  - output scaling factors (always "40,40" -- fixed at the real
//         7470A/7475A's standard resolution, 40 units/mm)
//   OO  - output options (fixed "0,1,0,0,1,0,0,0" -- matches the manual's
//         own real-hardware example: pen select yes, arcs/circles no)
//   OS  - output status byte -- tracks pen-down (bit 0) and the one-shot
//         "initialized" bit (bit 3, cleared after being read); "ready for
//         data" (bit 4) is always set
//   OW  - output window (the current IW clip rectangle)
//   TL  - tick length (percentage of the P1-P2 span, positive/negative
//         direction), used by XT/YT below
//   XT  - draw a small tick mark through the current pen position,
//         perpendicular to the X axis (i.e. a short vertical stroke),
//         without moving the pen
//   YT  - same as XT but perpendicular to the Y axis (a short horizontal
//         stroke)
//   IW  - input window: a clip rectangle (defaults to the whole area) applied to
//         every drawn line (PD segments, ticks, and label strokes) --
//         anything outside gets clipped or dropped entirely, matching the
//         real plotter's hard-clip behavior
//   SR  - character size, as a percentage of the P1-P2 span (width,height);
//         a new P1/P2 (IP) changes it too (SI's absolute size: not)
//   SI  - character size, in absolute centimeters (width,height) -- fixed
//         physical size regardless of P1/P2, via the same 40 units/mm
//         scale as OF
//   SL  - character slant (a shear factor applied to each glyph's x)
//   DI  - label direction, as a (run,rise) vector -- rotates subsequent
//         LB text (and CP's positioning) to match
//   CP  - character plot: repositions the "text cursor" by a number of
//         character widths/heights (from SR), along the current DI
//         direction, relative to the current pen position
//   LB  - label: draws text using a stroke ("stick") font (see
//         include/hpgl_font.h) terminated by DT's current terminator
//         (labelTerminator_, default ETX/0x03)
//   UC  - user-defined character -- explicitly a NOP per the manual for
//         the HP-IL interface variant (only HP-IB/RS-232-C actually draw
//         with it), so recognized-and-ignored is the *correct* behavior,
//         not a gap
//   DT  - define terminator: the very next character, taken completely
//         literally (per the manual, there's no "no parameter" case --
//         even ';' immediately after DT becomes the new terminator)
//   DF  - default: resets character size/slant/direction, tick length,
//         and the label terminator back to their power-on defaults --
//         unlike IN, does NOT touch pen position, segments, or IW
//
//   DF also turns user units (SC) off and plotting back to absolute.
//
//   LT  - line type 0..6[,pattern length %] for pen-down lines (PA/PD/PR,
//         circles, arcs); labels and ticks stay solid; bare LT: solid
//   SM  - symbol mode: a character drawn centred on every point (SM; off)
//   DR  - relative label direction (% of P1..P2's width/height)
//   VS, IM - accepted, nothing to do; CS/CA/SS/SA: set 0 only (others noted)
//   DP  - digitize: waits for a point -- touched in the Plotter view (the
//         real plotter: moved with the arrow keys, ENTER); OS bit 2 then
//         says it's ready, OD outputs it (x,y,pen), DC stops waiting
//
// A command that isn't implemented is ignored and logged once (since the
// last IN), trace or not: "* HP7470A: HP-GL 'LT' not supported -- ignored".
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
    // One character of HP-GL as if from the controller (the demo, demo.h)
    void put(char c) { feed_char(c); }

    ViewKind viewKind() const override { return ViewKind::Plotter; }
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
    // Main loop: a command left open at a line end (CR / LF) is carried
    // out when nothing more has come for kLineEndMs (see feed_char())
    void idle(void) override;

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

    // Digitizing (DP): waiting for a point (the Plotter view takes it from
    // a touch -- plotterview_digitizeTap()); digitize() gives it: the pen
    // moves there, OD reports it, OS bit 2 says one is ready
    bool digitizing() const { return digitizing_; }
    void digitize(std::int16_t x, std::int16_t y);
    void cancelDigitize() { digitizing_ = false; }   // (as DC)
    // A point in plotter units as user units (SC); false: no SC in use
    bool userUnits(double px, double py, double& ux, double& uy) const {
        if (!scaling_) return false;
        ux = scX1_ + (px - p1x_) * (scX2_ - scX1_) / (p2x_ - p1x_);
        uy = scY1_ + (py - p1y_) * (scY2_ - scY1_) / (p2y_ - p1y_);
        return true;
    }
    // The plotter's whole area in plotter units (a digitized point stays in it)
    static constexpr int kMaxX = 10300, kMaxY = 7650;
    // For the tests: what's waiting to be sent (an O.. answer), and the
    // commands not implemented seen since IN
    std::string outputText() const { return std::string(outQueue_.begin(), outQueue_.end()); }
    const std::string& unsupportedSeen() const { return unsupported_; }

private:
    enum class CoordMode { Absolute, Relative };

    void reset_state();
    // Resets just the drawing/label defaults DF is documented to affect
    // (character size/slant/direction, tick length, label terminator) --
    // used by both IN (full reset) and DF (defaults only, pen position
    // and plotted content untouched).
    void reset_defaults();
    void feed_char(char c);
    void execute(const std::string& cmd);
    std::vector<double> parseParams(const std::string& cmd, std::size_t from) const;
    void moveThroughParams(const std::vector<double>& params);
    void doMove(std::int16_t x, std::int16_t y);
    // A pen-down line in the current line type (LT): dashes/dots
    void patternedSegment(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1);
    void drawSymbol();                        // SM: symbol_ centred on the pen
    char symbol_ = '\0';                      // SM's character, '\0' = off
    int error_ = 0;                           // OE: 1 unknown instruction, 2 wrong number of parameters, 3 out of range
    int lineType_ = -1;                       // LT: -1 solid, 0..6
    double patternPct_ = 4.0;                 // pattern length, % of the P1..P2 diagonal
    double patPos_ = 0.0;                     // how far into the pattern the pen is
    // Plotter units per user unit (1,1 without SC)
    void unitFactors(double& fx, double& fy) const;
    // CI/AA/AR: from the pen, around (cx,cy) (plotter units), sweepDeg
    // degrees in chords of chordDeg; (vx,vy) = centre to pen, user units
    void arc(double cx, double cy, double vx, double vy, double sweepDeg, double chordDeg);
    void startVector(double cx, double cy, double& vx, double& vy) const;
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

    // One glyph's strokes (include/hpgl_font.h) with its origin at ox,oy:
    // scaled by charWidthUnits_/charHeightUnits_ (SR/SI), sheared by
    // charSlant_ (SL), rotated by dirCos_/dirSin_ (DI/DR)
    void drawGlyph(const char* strokes, double ox, double oy);
    void glyphToPlotter(double gx, double gy, double& px, double& py) const;
    // The strokes for a code in the set in use (CS/CA, SO/SI), nullptr: none
    const char* glyphFor(unsigned char code) const;

    // ── Tokenizer state (ported from pyILPER's process_char()) ──────────
    // 0: waiting for the command's first letter
    // 1: waiting for the second letter
    // 2: collecting numeric parameters until a non-parameter character
    // 3: inside LB label text, accumulating verbatim until the terminator
    //    (default ETX/0x03) -- see feed_char() in plotter.cpp
    int parseState_ = 0;
    std::string cmdBuf_;
    // CR / LF don't end a command (see feed_char()); one still open after
    // a line end is carried out by idle() once the controller has gone
    // quiet -- so "PU" as the last line of text from the HP-41 still
    // happens, while a line that goes on ("...,26" CR LF "46,...") isn't cut
    static constexpr std::uint32_t kLineEndMs = 300;
    bool lineEnded_ = false;
    std::uint64_t lineEndAtUs_ = 0;
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

    // Scaling points P1/P2 (IP), plotter units; user units (SC): x
    // scX1_..scX2_ and y scY1_..scY2_ map onto P1..P2 while scaling_
    std::int16_t p1x_ = 250, p1y_ = 279, p2x_ = 10250, p2y_ = 7479;
    bool scaling_ = false;
    double scX1_ = 0, scX2_ = 1, scY1_ = 0, scY2_ = 1;

    // Input window (clip rectangle) -- set by IW, defaults to the whole area
    // (0,0 .. kMaxX,kMaxY).
    std::int16_t iw1x_ = 0, iw1y_ = 0, iw2x_ = 0, iw2y_ = 0;

    // OS status-byte bit 3 ("initialized") -- true from reset until the
    // first OS read clears it (matches the real 7470A: power-on status is
    // 24 = 8+16, and bit 3 clears the moment it's actually read).
    bool everInitialized_ = true;
    // OS bit 1: P1/P2 changed (IP) since the last OS; bit 2: a digitized
    // point is ready (cleared by OD)
    bool p1p2Changed_ = false;
    bool digitizing_ = false;              // DP .. a point (or DC)
    bool pointReady_ = false;
    std::int16_t digX_ = 0, digY_ = 0;
    bool digPen_ = false;
    // Commands not implemented, already logged since the last IN (each is
    // logged once, whether trace is on or not): "LT SM "
    std::string unsupported_;

    // ── Label (LB) text state ────────────────────────────────────────
    // Character size in plotter units (SR, as a percentage of the P1-P2
    // span -- HP-GL defaults to 0.75%/1.5% width/height).
    double charWidthUnits_ = 0.0, charHeightUnits_ = 0.0;
    double charSlant_ = 0.0;                  // SL -- shear factor, default 0
    // SR's size in % of P1..P2 (charRelative_: SR in use, not SI) -- kept
    // so a new P1/P2 (IP) gives the characters their new size
    double charRelW_ = 0.75, charRelH_ = 1.5;
    bool charRelative_ = true;
    void applyRelativeSize();
    double dirCos_ = 1.0, dirSin_ = 0.0;       // DI -- default: horizontal
    // The CR point (where CR in a label returns to): set by every pen move
    // and DI/DR, moved by LF/VT in labels and by CP's lines. CP's spaces
    // wait in cpOff for the next label (from the CR point). See drawLabel().
    double crPosX_ = 0, crPosY_ = 0;
    double cpOffX_ = 0, cpOffY_ = 0;
    // Character sets (CS standard, CA alternate; SS/SA or SO/SI in a label
    // choose): 0 ASCII, 1..4 national (hpgl_font.h)
    int charSet_ = 0, altSet_ = 0;
    bool useAltSet_ = false;
    // Label terminator (DT) -- defaults to ETX (0x03). Per the manual, DT
    // ALWAYS takes the very next character literally as the new
    // terminator, with NO special case for "no parameter" -- only DF, IN,
    // or explicitly sending ETX itself as DT's argument resets it back.
    char labelTerminator_ = 0x03;

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
