#ifndef __HPGL_H__
#define __HPGL_H__

// HpglInterpreter -- HP-GL as a pen plotter carries it out: the
// instructions in, the drawing (segments(), as on the paper) and the
// answers to the output instructions (OA, OS, OE ...) out. No HP-IL, no
// display: a plotter device feeds it the bytes it receives and sends on
// what it answers (CPlotter, plotter.h, the HP 7470A on HP-IL); a view
// draws its segments (the Plotter view, plotterview.cpp). Another plotter
// (an HP 7475A, another interface) can use it as it is -- what differs
// between models (OI, OF, OO, the paper's size, P1/P2) is its HpglModel.
//
// The tokenizer (feed()) is ported from pyILPER's cls_HP7470Processor.
// The IW clip window and LB labels are ported from Mike's oo7470A.rx (an
// ooRexx HP7470A/HP-IL simulator, freeware under the Q Public License):
// its drawline:/plolet: routines (line clipping) and its chagra. stroke
// font (include/hpgl_font.h).
//
// The instructions (HP-GL as the HP 7470A has it -- not HP-GL/2):
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
//   OE  - output error: the last HP-GL error (1..8, the manual's table;
//         errorText()), 0 = none -- sets OS bit 5 and lights the ERROR
//         light; reading OE (or IN) clears both. Errors are set as the
//         manual has them: 1 unknown instruction, 2 wrong number of
//         parameters (PA/PD/PU/PR with an odd number, SI/SR/DI/DR/IP/IW/
//         SC/CP/CI/AA/AR), 3 out of range (coordinates and pens beyond
//         -32768..32767, sizes/slant/ticks beyond -128..127.9999, VS, LT,
//         DI/DR 0,0, CS/CA), 6 position overflow (CP)
//   IM  - input mask: the E-mask -- which errors are kept at all (default
//         223, every one but 6; IN and DF set it back)
//   OI  - output identification (the model's: "7470A")
//   OF  - output scaling factors (the model's units per mm: "40,40")
//   OO  - output options (the model's: "0,1,0,0,1,0,0,0" for the 7470A,
//         the manual's own example: pen select yes, arcs/circles no)
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
//   VS  - pen speed: checked (0..96.9999), nothing to do
//   AP AF AH EC VA VN - the 9872's instructions the 7470A ignores: no error
//   DP  - digitize: waits for a point -- touched in the Plotter view (the
//         real plotter: moved with the arrow keys, ENTER); OS bit 2 then
//         says it's ready, OD outputs it (x,y,pen), DC stops waiting
//
// A command that isn't implemented is ignored (error 1) and logged once
// (since the last IN), trace or not: "* TFPLOT: HP-GL 'ZZ' not supported --
// ignored".
//

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One line drawn on the paper (plotter units)
struct PlotSegment {
    std::int16_t x0, y0, x1, y1;
    std::uint8_t pen;
    bool wide;                  // drawn with the wide tip (setWideTip())
};

// What makes a plotter model different, as far as HP-GL goes
struct HpglModel {
    const char* id;                          // OI's answer: "7470A"
    int unitsPerMm;                          // OF's answer (both axes); SI's sizes
    const char* options;                     // OO's answer
    std::int16_t maxX, maxY;                 // the whole area (the paper), plotter units
    std::int16_t p1x, p1y, p2x, p2y;         // the factory P1/P2 (IN, a bare IP)
};
// The HP 7470A (and its manual's factory P1/P2)
inline constexpr HpglModel kHp7470A = { "7470A", 40, "0,1,0,0,1,0,0,0", 10300, 7650, 250, 279, 10250, 7479 };

class HpglInterpreter {
public:
    // name: for the log ("TFPLOT"); model: OI/OF/OO and the paper
    explicit HpglInterpreter(const char* name, const HpglModel& model = kHp7470A);

    const HpglModel& model() const { return model_; }

    // One character of HP-GL, as it comes from the controller
    void put(char c) { feed(c); }
    // Now and then (the main loop): a command left open at a line end
    // (CR / LF) is carried out when nothing more has come for kLineEndMs
    void idle();
    // Everything back as after IN (and a device clear): the paper empty
    void reset();

    // ── Hooks ─────────────────────────────────────────────────────────
    // An output instruction's answer ("24\r\n"), to send to the controller
    void setOutputCallback(std::function<void(const std::string&)> cb) { onOutput_ = std::move(cb); }
    // IN or reset(): what the device itself has to start afresh
    void setResetCallback(std::function<void()> cb) { onReset_ = std::move(cb); }
    // A line drawn (already in segments()) -- the view draws it
    void setDrawCallback(std::function<void(const PlotSegment&)> cb) { onDraw_ = std::move(cb); }
    // The paper emptied (IN, reset()) -- the view clears
    void setClearCallback(std::function<void()> cb) { onClear_ = std::move(cb); }

    // ── The paper and the pen ─────────────────────────────────────────
    const std::vector<PlotSegment>& segments() const { return segments_; }
    std::int16_t penX() const { return penX_; }
    std::int16_t penY() const { return penY_; }
    bool penDown() const { return penDown_; }
    std::uint8_t currentPen() const { return currentPen_; }
    // The pen's tip, as if a pen with a broader tip were put in: only
    // what's drawn from now on gets it (each segment keeps its own), so
    // thin and wide lines mix in one plot. Thin at start-up; IN and
    // clearing don't change it. Only the view uses it (Plotter -> Pen).
    void setWideTip(bool wide) { wideTip_ = wide; }
    bool wideTip() const { return wideTip_; }

    // OS's status byte: 1 pen down, 2 P1/P2 changed, 4 a digitized point,
    // 8 initialized, 16 ready for data (always), 32 error -- without
    // clearing anything (HP-IL's SST asks for it this way)
    int statusByte() const;

    // ── Digitizing (DP) ───────────────────────────────────────────────
    // Waiting for a point (the Plotter view takes it from a touch --
    // views_digitizeTap()); digitize() gives it: the pen moves there, OD
    // reports it, OS bit 2 says one is ready
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

    // ── Errors (OE) and the ERROR light ───────────────────────────────
    // The last HP-GL error, as OE reports it (0: none) -- also the front
    // panel's ERROR light: lit while it isn't 0. Only errors the input
    // mask lets through (IM's E-mask, default 223: all but error 6) are
    // kept at all, as on the real plotter. OE and IN put it out.
    int errorCode() const { return error_; }
    // What an error number means (OE's table): "bad parameter"
    static const char* errorText(int code);
    // ... in short, for a narrow row: "unknown instruction"
    static const char* errorShort(int code);
    // A row in Plotter -> Errors, at most `width` characters: the number,
    // what it means (short) and the command -- the command cut ("~") if
    // need be, never the meaning: "1 unknown instruction: RO 90"
    static std::string errorRow(int code, const std::string& command, std::size_t width);
    // The light put out from HIPI's menu (Plotter -> Errors -> Clear
    // error): as if OE had been read, without the answer
    void clearError() { error_ = 0; ++errorSerial_; }
    // The last errors (newest last; up to kErrorLogSize), with the command
    // that caused them -- for the Plotter view. Kept over IN and clearing.
    struct ErrorEntry {
        std::uint8_t code;
        char command[28];                  // "SC 0,100,0" (shortened)
    };
    static constexpr std::size_t kErrorLogSize = 8;
    const std::vector<ErrorEntry>& errorLog() const { return errorLog_; }
    // Changes whenever an error comes or goes (the view redraws then)
    std::uint32_t errorSerial() const { return errorSerial_; }

    // The commands not implemented seen since IN (each logged once): "QQ QR "
    const std::string& unsupportedSeen() const { return unsupported_; }

    // Extended trace: pen, position, mode, parser state
    void show() const;

private:
    enum class CoordMode { Absolute, Relative };

    void resetState();
    // Resets just the drawing/label defaults DF is documented to affect
    // (character size/slant/direction, tick length, label terminator) --
    // used by both IN (full reset) and DF (defaults only, pen position
    // and plotted content untouched).
    void resetDefaults();
    void feed(char c);
    void execute(const std::string& cmd);
    void executeCommand(const std::string& cmd, const std::string& mnemonic,
                        const std::vector<double>& params);
    std::vector<double> parseParams(const std::string& cmd, std::size_t from) const;
    void moveThroughParams(const std::vector<double>& params);
    void doMove(std::int16_t x, std::int16_t y);
    // A pen-down line in the current line type (LT): dashes/dots
    void patternedSegment(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1);
    void drawSymbol();                        // SM: symbol_ centred on the pen
    // An HP-GL error in the command being carried out (cmd_): kept if
    // IM's E-mask lets it through (bit n-1 for error n)
    void setError(int code);
    void output(const std::string& s);        // an O.. instruction's answer
    // Plotter units per user unit (1,1 without SC)
    void unitFactors(double& fx, double& fy) const;
    // CI/AA/AR: from the pen, around (cx,cy) (plotter units), sweepDeg
    // degrees in chords of chordDeg; (vx,vy) = centre to pen, user units
    void arc(double cx, double cy, double vx, double vy, double sweepDeg, double chordDeg);
    void startVector(double cx, double cy, double& vx, double& vy) const;

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

    // Cohen-Sutherland line clipping against [iw1x_,iw1y_]-[iw2x_,iw2y_],
    // within the paper (0,0 .. the model's maxX,maxY: the hard-clip limits).
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
    void applyRelativeSize();

    std::string name_;
    HpglModel model_;

    // ── Tokenizer state (ported from pyILPER's process_char()) ──────────
    // 0: waiting for the command's first letter
    // 1: waiting for the second letter
    // 2: collecting numeric parameters until a non-parameter character
    // 3: inside LB label text, accumulating verbatim until the terminator
    //    (default ETX/0x03)
    // 4: DT's character, 5: SM's character
    int parseState_ = 0;
    std::string cmdBuf_;
    // CR / LF don't end a command (see feed()); one still open after a
    // line end is carried out by idle() once the controller has gone
    // quiet -- so "PU" as the last line of text from the HP-41 still
    // happens, while a line that goes on ("...,26" CR LF "46,...") isn't cut
    static constexpr std::uint32_t kLineEndMs = 300;
    bool lineEnded_ = false;
    std::uint64_t lineEndAtUs_ = 0;
    bool inParam_ = false;
    bool sawSeparator_ = false;
    int numParam_ = 0;
    const std::string* cmd_ = nullptr;        // the command execute() is on (for the error log)

    // ── Plotter state ─────────────────────────────────────────────────
    std::int16_t penX_ = 0, penY_ = 0;
    bool penDown_ = false;
    std::uint8_t currentPen_ = 1;
    bool wideTip_ = false;      // (setWideTip())
    CoordMode coordMode_ = CoordMode::Absolute;
    char symbol_ = '\0';                      // SM's character, '\0' = off
    int lineType_ = -1;                       // LT: -1 solid, 0..6
    double patternPct_ = 4.0;                 // pattern length, % of the P1..P2 diagonal
    double patPos_ = 0.0;                     // how far into the pattern the pen is

    // Tick length as a percentage of the P1-P2 span, in the positive and
    // negative directions -- set by TL, used by XT/YT. HP-GL default: 0.5
    // for both.
    double tickLenPos_ = 0.5, tickLenNeg_ = 0.5;

    // Scaling points P1/P2 (IP), plotter units; user units (SC): x
    // scX1_..scX2_ and y scY1_..scY2_ map onto P1..P2 while scaling_
    std::int16_t p1x_ = 0, p1y_ = 0, p2x_ = 0, p2y_ = 0;
    bool scaling_ = false;
    double scX1_ = 0, scX2_ = 1, scY1_ = 0, scY2_ = 1;

    // Input window (clip rectangle) -- set by IW, defaults to the whole area
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
    // Commands not implemented, already logged since the last IN: "LT SM "
    std::string unsupported_;

    // ── Errors ────────────────────────────────────────────────────────
    int error_ = 0;                           // OE's error number (errorText()), 0 = none
    int errorMask_ = 223;                     // IM's E-mask (IN, DF: 223)
    std::vector<ErrorEntry> errorLog_;
    std::uint32_t errorSerial_ = 0;

    // ── Label (LB) text state ────────────────────────────────────────
    // Character size in plotter units (SR, as a percentage of the P1-P2
    // span -- HP-GL defaults to 0.75%/1.5% width/height).
    double charWidthUnits_ = 0.0, charHeightUnits_ = 0.0;
    double charSlant_ = 0.0;                  // SL -- shear factor, default 0
    // SR's size in % of P1..P2 (charRelative_: SR in use, not SI) -- kept
    // so a new P1/P2 (IP) gives the characters their new size
    double charRelW_ = 0.75, charRelH_ = 1.5;
    bool charRelative_ = true;
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
    std::function<void(const std::string&)> onOutput_;
    std::function<void()> onReset_;
    std::function<void(const PlotSegment&)> onDraw_;
    std::function<void()> onClear_;
};

#endif//__HPGL_H__
