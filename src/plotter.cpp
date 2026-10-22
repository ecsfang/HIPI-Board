#define MODULE "PLOTTER"

#include "plotter.h"
#include "hpgl_font.h"
#include "pico/time.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>


// Real 7475A factory-default scaling points P1/P2 (IN, bare IP) -- also
// IW's default clip window.
namespace {
constexpr int kP1X = 250, kP1Y = 279, kP2X = 10250, kP2Y = 7479;
std::int16_t clamp16(double v) {
    return static_cast<std::int16_t>(v < -32768.0 ? -32768.0 : v > 32767.0 ? 32767.0 : v);
}
// Safety cap for LB label text accumulation (see feed_char()'s case 3) --
// well beyond any realistic real-world label, just a backstop against a
// program that forgets the terminator entirely.
constexpr std::size_t kMaxLabelLength = 200;
}

void CPlotter::reset_defaults() {
    // Everything DF is documented to reset -- see execute()'s "DF" branch.
    tickLenPos_ = tickLenNeg_ = 0.5;  // HP-GL default: 0.5% of the P1-P2 span
    // SR defaults: 0.75%/1.5% of the P1-P2 span (HP-GL default character
    // size), converted to plotter units up front so drawGlyph() never
    // has to redo the percentage math per character.
    charRelW_ = 0.75;
    charRelH_ = 1.5;
    charRelative_ = true;
    applyRelativeSize();
    charSlant_ = 0.0;
    dirCos_ = 1.0; dirSin_ = 0.0;
    labelTerminator_ = 0x03;  // ETX -- see plotter.h's note on DT/DF
}

void CPlotter::applyRelativeSize() {
    // SR's size is a share of P1..P2: a new P1/P2 (IP) changes it too
    if (!charRelative_) return;
    charWidthUnits_  = charRelW_ / 100.0 * (p2x_ - p1x_);
    charHeightUnits_ = charRelH_ / 100.0 * (p2y_ - p1y_);
}

void CPlotter::reset_state() {
    parseState_ = 0;
    cmdBuf_.clear();
    lineEnded_ = false;
    inParam_ = false;
    sawSeparator_ = false;

    penX_ = penY_ = 0;
    penDown_ = false;
    currentPen_ = 1;
    coordMode_ = CoordMode::Absolute;

    // P1/P2 back to the factory default, user units (SC) off
    p1x_ = kP1X; p1y_ = kP1Y; p2x_ = kP2X; p2y_ = kP2Y;
    scaling_ = false;

    // IW defaults to the plotter's whole area (as the real one: text from
    // x=100, left of P1, isn't cut)
    iw1x_ = 0; iw1y_ = 0; iw2x_ = kMaxX; iw2y_ = kMaxY;

    everInitialized_ = true;  // OS's power-on status bit, see plotter.h
    p1p2Changed_ = false;
    error_ = 0;
    symbol_ = '\0';
    lineType_ = -1;
    patternPct_ = 4.0;
    patPos_ = 0.0;
    digitizing_ = false;
    pointReady_ = false;
    unsupported_.clear();

    reset_defaults();
    crPosX_ = penX_; crPosY_ = penY_;
    cpOffX_ = cpOffY_ = 0.0;
    charSet_ = altSet_ = 0;
    useAltSet_ = false;

    segments_.clear();

    outQueue_.clear();
    outEnd_ = true;
    midTransfer_ = false;
}

void CPlotter::idle(void) {
    // The text ended in the middle of a command (no ';' or next command
    // yet) and nothing more came: it's complete
    if (!lineEnded_ || parseState_ != 2 || time_us_64() < lineEndAtUs_) return;
    lineEnded_ = false;
    execute(cmdBuf_);
    cmdBuf_.clear();
    parseState_ = 0;
}

void CPlotter::digitize(std::int16_t x, std::int16_t y) {
    if (!digitizing_) return;
    x = static_cast<std::int16_t>(std::max(0, std::min<int>(x, kMaxX)));
    y = static_cast<std::int16_t>(std::max(0, std::min<int>(y, kMaxY)));
    doMove(x, y);                             // the pen goes there (drawing if it's down)
    digX_ = x;
    digY_ = y;
    digPen_ = penDown_;
    digitizing_ = false;
    pointReady_ = true;
    LOGF("\r\n * %s: digitized point %d,%d", name(), x, y);
}

void CPlotter::clear(void) {
    MTRC_LOGF("clear (DCL/SDC)");
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

void CPlotter::queueOutput(const std::string& s) {
    outQueue_.assign(s.begin(), s.end());
    outEnd_ = false;
    MTRC_LOGF("queued response: \"%s\"", s.c_str());
}

// Tokenizer, ported from pyILPER's cls_HP7470Processor.process_char().
void CPlotter::feed_char(char c) {
    // CR and LF are ignored outside a label (and DT's argument), as on the
    // real plotter -- even in the middle of a number: a controller that
    // sends a long PA line in pieces puts CR LF wherever its line ends
    // ("...,26" CR LF "46,..." is 2646). They used to end the command,
    // and the rest of the line was lost.
    if ((c == '\r' || c == '\n') && parseState_ != 3 && parseState_ != 4) {
        if (parseState_ == 2) {                  // (an open command: see idle())
            lineEnded_ = true;
            lineEndAtUs_ = time_us_64() + kLineEndMs * 1000ull;
        }
        return;
    }
    lineEnded_ = false;
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
            if (cmdBuf_ == "LB") {
                // Label text starts immediately after the mnemonic and
                // isn't HP-GL syntax at all -- it must NOT go through the
                // normal numeric-parameter state (case 2 below), since
                // label text is extremely often digits/commas/periods/
                // signs (e.g. axis labels like "0,00" or "-180"), which
                // would otherwise get misparsed as HP-GL parameters
                // before we ever noticed this was LB. Jump straight to
                // raw verbatim accumulation instead.
                cmdBuf_.clear();
                parseState_ = 3;
                return;
            }
            if (cmdBuf_ == "DT") {
                // Define Terminator's argument is a single literal
                // character, taken completely as-is -- it must NOT go
                // through normal numeric-parameter parsing either (the
                // argument could be any byte, digit or not, and per the
                // manual there's no "no argument" case to special-case).
                parseState_ = 4;
                return;
            }
            if (cmdBuf_ == "SM") {
                // Symbol Mode's argument is one character too (as DT)
                parseState_ = 5;
                return;
            }
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
            // A sign right after a number starts the next one ("CP-9.-1" is
            // -9 and -1)
            if (inParam_ && (c == '+' || c == '-')) {
                inParam_ = false;
                sawSeparator_ = false;
            }
            if (!inParam_) {
                inParam_ = true;
                // A comma already there, or one put in (a space or sign
                // between numbers); the comma counts for this number only
                if (!sawSeparator_ && numParam_ > 0) cmdBuf_ += ',';
                ++numParam_;
                sawSeparator_ = false;
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
        // Anything else (a letter, ';', etc.) ends this command's
        // parameter portion and starts the next one. (LB is handled
        // earlier, at the state1->state3 transition -- see above --
        // since its "parameters" are actually raw label text, not HP-GL
        // syntax, and must never reach here.)
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
        // Inside LB label text -- accumulate verbatim until the terminator
        // (labelTerminator_, defaulting to ETX/0x03 -- see DT).
        if (c == labelTerminator_) {
            drawLabel(cmdBuf_);
            cmdBuf_.clear();
            parseState_ = 0;
        } else {
            cmdBuf_ += c;
            // Safety cap: a program that forgets the terminator (e.g.
            // sends ';' -- an ordinary HP-GL separator, but not what LB
            // expects) would otherwise make this buffer swallow every
            // subsequent byte forever, including what should have been
            // real commands, growing without bound. Bail out once a
            // label's gotten implausibly long for anything real rather
            // than let that happen.
            if (cmdBuf_.size() > kMaxLabelLength) {
                if (bTrace) {
                    MLOGF("LB exceeded %zu chars without a terminator "
                         "(0x%02X) -- aborting, treating as malformed",
                         kMaxLabelLength, static_cast<unsigned char>(labelTerminator_));
                }
                cmdBuf_.clear();
                parseState_ = 0;
            }
        }
        break;

    case 4:
        // DT's single-character argument -- taken completely literally,
        // whatever it is (no special case for ';' or anything else; see
        // the manual note quoted in plotter.h).
        labelTerminator_ = c;
        cmdBuf_.clear();
        parseState_ = 0;
        break;

    case 5:
        // SM's character: drawn at every point from now on; ';' (or a
        // control character / space) turns symbol mode off
        symbol_ = (c == ';' || static_cast<unsigned char>(c) <= ' ') ? '\0' : c;
        MTRC_LOGF("SM '%c'", symbol_ ? symbol_ : ' ');
        cmdBuf_.clear();
        parseState_ = 0;
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

// ── Clipping (ported from oo7470A.rx's drawline: -- Cohen-Sutherland) ──────

namespace {
constexpr int kInside = 0, kLeft = 1, kRight = 2, kBottom = 4, kTop = 8;

int outcode(double x, double y, double xmin, double ymin, double xmax, double ymax) {
    int code = kInside;
    if (x < xmin) code |= kLeft;
    else if (x > xmax) code |= kRight;
    if (y < ymin) code |= kBottom;
    else if (y > ymax) code |= kTop;
    return code;
}
}  // namespace

bool CPlotter::clipToWindow(double& x0, double& y0, double& x1, double& y1) const {
    double xmin = iw1x_, xmax = iw2x_;
    double ymin = iw1y_, ymax = iw2y_;
    if (xmin > xmax) std::swap(xmin, xmax);
    if (ymin > ymax) std::swap(ymin, ymax);

    int code0 = outcode(x0, y0, xmin, ymin, xmax, ymax);
    int code1 = outcode(x1, y1, xmin, ymin, xmax, ymax);

    for (;;) {
        if (!(code0 | code1)) return true;    // both endpoints inside
        if (code0 & code1) return false;      // trivially outside -- nothing to draw

        const int codeOut = code0 ? code0 : code1;
        double x = 0, y = 0;
        if (codeOut & kTop) {
            x = x0 + (x1 - x0) * (ymax - y0) / (y1 - y0);
            y = ymax;
        } else if (codeOut & kBottom) {
            x = x0 + (x1 - x0) * (ymin - y0) / (y1 - y0);
            y = ymin;
        } else if (codeOut & kRight) {
            y = y0 + (y1 - y0) * (xmax - x0) / (x1 - x0);
            x = xmax;
        } else {  // kLeft
            y = y0 + (y1 - y0) * (xmin - x0) / (x1 - x0);
            x = xmin;
        }

        if (codeOut == code0) {
            x0 = x; y0 = y;
            code0 = outcode(x0, y0, xmin, ymin, xmax, ymax);
        } else {
            x1 = x; y1 = y;
            code1 = outcode(x1, y1, xmin, ymin, xmax, ymax);
        }
    }
}

void CPlotter::emitSegment(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1) {
    double cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
    if (!clipToWindow(cx0, cy0, cx1, cy1)) return;  // fully outside IW -- nothing to draw

    const auto ix0 = static_cast<std::int16_t>(cx0);
    const auto iy0 = static_cast<std::int16_t>(cy0);
    const auto ix1 = static_cast<std::int16_t>(cx1);
    const auto iy1 = static_cast<std::int16_t>(cy1);

    segments_.push_back({ix0, iy0, ix1, iy1, currentPen_, wideTip_});
    if (bTrace) {
        MLOGF("  draw (%d,%d) -> (%d,%d) pen=%u", ix0, iy0, ix1, iy1, currentPen_);
    }
    if (onDraw_) onDraw_(ix0, iy0, ix1, iy1, currentPen_);
}

// Line types (LT 1..6), as on the HP 7470A: the pattern in % of its
// length -- draw, gap, draw, gap ... (a 0 % draw is a dot)
namespace {
struct LinePattern { int n; std::uint8_t part[6]; };
const LinePattern kLinePatterns[7] = {
    { 0, {} },                                // 0: dots at the points (patternedSegment())
    { 2, { 0, 100 } },                        // 1: a dot at each pattern's start
    { 2, { 50, 50 } },                        // 2: half dash
    { 2, { 70, 30 } },                        // 3: long dash
    { 4, { 80, 10, 0, 10 } },                 // 4: dash dot
    { 4, { 70, 10, 10, 10 } },                // 5: long and short dash
    { 6, { 50, 10, 10, 10, 10, 10 } },        // 6: dash and two short ones
};
}

void CPlotter::patternedSegment(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (lineType_ == 0) {                     // dots at the points only
        if (patPos_ == 0.0) emitSegment(x0, y0, x0, y0);
        emitSegment(x1, y1, x1, y1);
        patPos_ = 1.0;
        return;
    }
    const LinePattern& p = kLinePatterns[lineType_];
    const double pw = p1x_ - p2x_, ph = p1y_ - p2y_;
    const double patLen = std::max(1.0, patternPct_ / 100.0 * std::sqrt(pw * pw + ph * ph));
    // Walk the segment through the pattern; patPos_ (units into the
    // pattern) carries on into the next segment while the pen stays down
    double done = 0.0;
    while (done < len) {
        double at = 0.0;
        int part = 0;
        for (; part < p.n; ++part) {
            const double partLen = p.part[part] / 100.0 * patLen;
            if (patPos_ < at + partLen || (partLen == 0.0 && patPos_ == at)) break;
            at += partLen;
        }
        if (part >= p.n) { patPos_ = 0.0; continue; }   // the next pattern
        const double partLen = p.part[part] / 100.0 * patLen;
        const double step = std::min(len - done, at + partLen - patPos_);
        if ((part & 1) == 0) {                // a draw part (a 0 % one: a dot)
            const double a = done / len, b = (done + step) / len;
            emitSegment(clamp16(std::floor(x0 + dx * a + 0.5)), clamp16(std::floor(y0 + dy * a + 0.5)),
                        clamp16(std::floor(x0 + dx * b + 0.5)), clamp16(std::floor(y0 + dy * b + 0.5)));
        }
        done += step;
        patPos_ += step;
        if (partLen == 0.0) patPos_ += 1e-9;  // (past the dot)
    }
}

void CPlotter::doMove(std::int16_t x, std::int16_t y) {
    if (penDown_) {
        MTRC_LOGF("  (pen down move)");
        if (lineType_ < 0) emitSegment(penX_, penY_, x, y);
        else patternedSegment(penX_, penY_, x, y);
    } else {
        MTRC_LOGF("  move -> (%d,%d)", x, y);
        patPos_ = 0.0;                        // (a new line: the pattern starts again)
        if (onMove_) onMove_(x, y);
    }
    penX_ = x;
    penY_ = y;
    crPosX_ = x;                              // (a move sets the CR point, see drawLabel())
    crPosY_ = y;
}

void CPlotter::drawTick(bool vertical) {
    // tp/tn are percentages of the RELEVANT axis's P1-P2 span: XT (a
    // vertical stroke) uses the Y span, YT (a horizontal stroke) uses the
    // X span -- the tick crosses the axis it's perpendicular to.
    const int spanY = p2y_ - p1y_;
    const int spanX = p2x_ - p1x_;
    std::int16_t x0, y0, x1, y1;
    if (vertical) {
        const int lenPos = static_cast<int>(tickLenPos_ / 100.0 * spanY);
        const int lenNeg = static_cast<int>(tickLenNeg_ / 100.0 * spanY);
        x0 = x1 = penX_;
        y0 = static_cast<std::int16_t>(penY_ - lenNeg);
        y1 = static_cast<std::int16_t>(penY_ + lenPos);
    } else {
        const int lenPos = static_cast<int>(tickLenPos_ / 100.0 * spanX);
        const int lenNeg = static_cast<int>(tickLenNeg_ / 100.0 * spanX);
        y0 = y1 = penY_;
        x0 = static_cast<std::int16_t>(penX_ - lenNeg);
        x1 = static_cast<std::int16_t>(penX_ + lenPos);
    }
    // A tick always draws, using the current pen, regardless of the
    // logical PU/PD state -- and leaves the pen position unchanged
    // afterward (it's a one-shot mark, not a move).
    emitSegment(x0, y0, x1, y1);
}

void CPlotter::moveThroughParams(const std::vector<double>& params) {
    // Coordinates come in (x,y) pairs -- an odd trailing value (malformed
    // input) is ignored. With SC they're user units: mapped onto P1..P2
    // (relative ones: just scaled) and rounded to the nearest plotter unit;
    // without, plotter units as given (a fraction dropped).
    for (std::size_t i = 0; i + 1 < params.size(); i += 2) {
        double x = params[i], y = params[i + 1];
        if (scaling_) {
            const double fx = (p2x_ - p1x_) / (scX2_ - scX1_), fy = (p2y_ - p1y_) / (scY2_ - scY1_);
            if (coordMode_ == CoordMode::Absolute) {
                x = p1x_ + (x - scX1_) * fx;
                y = p1y_ + (y - scY1_) * fy;
            } else {
                x *= fx;
                y *= fy;
            }
            x = std::floor(x + 0.5);
            y = std::floor(y + 0.5);
        }
        if (coordMode_ == CoordMode::Relative) {
            x += penX_;
            y += penY_;
        }
        doMove(clamp16(x), clamp16(y));
        if (symbol_ != '\0') drawSymbol();
    }
}

void CPlotter::drawSymbol() {
    // SM: the character centred on the pen (on its strokes' centre, as
    // the real plotter), in the label size and direction; the pen and the
    // CR point stay
    const char* strokes = glyphFor(static_cast<unsigned char>(symbol_));
    if (strokes == nullptr) return;
    const std::vector<double> nums = parseParams(strokes, 0);
    double cx = 0, cy = 0;
    int n = 0;
    for (std::size_t i = 0; i + 2 < nums.size(); i += 3, ++n) { cx += nums[i + 1]; cy += nums[i + 2]; }
    if (n == 0) return;
    double gx = 0, gy = 0;
    glyphToPlotter(cx / n, cy / n, gx, gy);    // (the centre's offset from the origin)
    drawGlyph(strokes, penX_ - gx, penY_ - gy);
}

// ── Circles and arcs (CI, AA, AR) ───────────────────────────────────────

void CPlotter::unitFactors(double& fx, double& fy) const {
    if (scaling_) {
        fx = (p2x_ - p1x_) / (scX2_ - scX1_);
        fy = (p2y_ - p1y_) / (scY2_ - scY1_);
    } else {
        fx = fy = 1.0;
    }
}

void CPlotter::arc(double cx, double cy, double vx, double vy, double sweepDeg, double chordDeg) {
    // The angle is turned in user units (vx,vy: from the centre to the
    // start), each chord end mapped to plotter units -- with unequal x/y
    // scaling a circle comes out as an ellipse, as on the real plotter
    double fx, fy;
    unitFactors(fx, fy);
    chordDeg = std::fabs(chordDeg);
    if (chordDeg < 0.5) chordDeg = 0.5;              // the plotter's range: 0.5 .. 180
    if (chordDeg > 180.0) chordDeg = 180.0;
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(sweepDeg) / chordDeg - 1e-9)));
    const double step = sweepDeg / n * 3.14159265358979323846 / 180.0;
    for (int i = 1; i <= n; ++i) {
        const double a = step * i, c = std::cos(a), s = std::sin(a);
        const double ux = vx * c - vy * s, uy = vx * s + vy * c;
        doMove(clamp16(std::floor(cx + ux * fx + 0.5)), clamp16(std::floor(cy + uy * fy + 0.5)));
    }
}

// The pen's distance from a centre (plotter units) as a user-unit vector
void CPlotter::startVector(double cx, double cy, double& vx, double& vy) const {
    double fx, fy;
    unitFactors(fx, fy);
    vx = (penX_ - cx) / fx;
    vy = (penY_ - cy) / fy;
}

// ── Label (LB) text rendering ───────────────────────────────────────────
// As the HP 7470A (and pyILPER's emu7470): characters advance a cell of
// 1.5 widths, lines are 2 heights. The CR point -- where CR returns to --
// is set by every pen move (PA/PR/PU/PD, arcs) and DI/DR, and moved by LF,
// VT and CP's lines; CP's spaces are kept for the next label, counted from
// the CR point. After a label the pen stands where the next character
// would go.

void CPlotter::glyphToPlotter(double gx, double gy, double& px, double& py) const {
    double sx = charWidthUnits_ * gx / 32.0;
    const double sy = charHeightUnits_ * gy / 32.0;
    sx += charSlant_ * sy;                            // SL: shear x by slant*y
    px = sx * dirCos_ - sy * dirSin_;                 // DI: rotate into plotter space
    py = sx * dirSin_ + sy * dirCos_;
}

const char* CPlotter::glyphFor(unsigned char code) const {
    const int set = useAltSet_ ? altSet_ : charSet_;
    if (set >= 1 && set <= 4) {
        if (const char* g = hpgl_font::nationalGlyph(set, code)) return g;
    }
    return code < 128 ? hpgl_font::kGlyphs[code] : nullptr;
}

void CPlotter::drawGlyph(const char* strokes, double ox, double oy) {
    const std::vector<double> nums = parseParams(strokes, 0);
    std::int16_t prevX = 0, prevY = 0;
    bool first = true;
    for (std::size_t i = 0; i + 2 < nums.size(); i += 3) {
        double rx, ry;
        glyphToPlotter(nums[i + 1], nums[i + 2], rx, ry);
        const std::int16_t x = clamp16(std::floor(ox + rx + 0.5));
        const std::int16_t y = clamp16(std::floor(oy + ry + 0.5));
        if (!first && nums[i] != 0.0) emitSegment(prevX, prevY, x, y);
        prevX = x; prevY = y;
        first = false;
    }
}

void CPlotter::drawLabel(const std::string& text) {
    MTRC_LOGF("LB \"%s\"", text.c_str());
    const double sp = 1.5 * charWidthUnits_, ln = 2.0 * charHeightUnits_;
    const double cdx = sp * dirCos_, cdy = sp * dirSin_;     // one character on
    const double ldx = ln * dirSin_, ldy = -ln * dirCos_;    // one line down
    double rx, ry;
    if (cpOffX_ != 0.0 || cpOffY_ != 0.0) {   // CP's spaces: from the CR point
        rx = crPosX_ + cpOffX_;
        ry = crPosY_ + cpOffY_;
        cpOffX_ = cpOffY_ = 0.0;
    } else {
        rx = penX_;
        ry = penY_;
    }
    for (char ch : text) {
        const unsigned char code = static_cast<unsigned char>(ch);
        switch (code) {
        case ' ':  rx += cdx; ry += cdy; break;
        case 0x0D: rx = crPosX_; ry = crPosY_; break;                       // CR
        case 0x0A: crPosX_ += ldx; crPosY_ += ldy; rx += ldx; ry += ldy; break;   // LF
        case 0x08: rx -= cdx; ry -= cdy; break;                             // BS
        case 0x09: rx -= 0.5 * cdx; ry -= 0.5 * cdy; break;                 // HT: half a space back
        case 0x0B: crPosX_ -= ldx; crPosY_ -= ldy; rx -= ldx; ry -= ldy; break;   // VT: a line up
        case 0x0E: useAltSet_ = true; break;                                // SO: alternate set
        case 0x0F: useAltSet_ = false; break;                               // SI: standard set
        default: {
            const int set = useAltSet_ ? altSet_ : charSet_;
            if (code > ' ' && hpgl_font::nationalNonSpacing(set, code)) { rx -= cdx; ry -= cdy; }
            const char* strokes = code > ' ' ? glyphFor(code) : nullptr;
            if (strokes != nullptr) drawGlyph(strokes, rx, ry);
            rx += cdx;                        // (other control characters: a space)
            ry += cdy;
            break;
        }
        }
    }
    // The pen ends where the next character would go (pen up)
    penX_ = clamp16(std::floor(rx + 0.5));
    penY_ = clamp16(std::floor(ry + 0.5));
    if (onMove_ && !penDown_) onMove_(penX_, penY_);
}

void CPlotter::execute(const std::string& cmd) {
    if (cmd.size() < 2) return;
    const std::string mnemonic = cmd.substr(0, 2);
    const std::vector<double> params = parseParams(cmd, 2);

    if (mnemonic == "IN") {
        MTRC_LOGF("IN (initialize/reset)");
        reset_state();
        if (onClear_) onClear_();
    } else if (mnemonic == "SP") {
        currentPen_ = params.empty() ? 0 : static_cast<std::uint8_t>(params[0]);
        MTRC_LOGF("SP -> pen=%u", currentPen_);
        if (onPenChanged_) onPenChanged_(currentPen_, penDown_);
    } else if (mnemonic == "PU") {
        penDown_ = false;
        if (bTrace) {
            MLOGF("PU (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        if (onPenChanged_) onPenChanged_(currentPen_, penDown_);
        moveThroughParams(params);
    } else if (mnemonic == "PD") {
        penDown_ = true;
        if (bTrace) {
            MLOGF("PD (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        if (onPenChanged_) onPenChanged_(currentPen_, penDown_);
        moveThroughParams(params);
    } else if (mnemonic == "PA") {
        coordMode_ = CoordMode::Absolute;
        if (bTrace) {
            MLOGF("PA absolute (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        moveThroughParams(params);
    } else if (mnemonic == "PR") {
        coordMode_ = CoordMode::Relative;
        if (bTrace) {
            MLOGF("PR relative (%zu point%s)", params.size() / 2,
                 params.size() == 2 ? "" : "s");
        }
        moveThroughParams(params);
    } else if (mnemonic == "OP") {
        // Output P1/P2 -- the scaling points (IP; factory defaults after
        // IN). The HP-41's PINIT ROM routine requires this response to
        // complete its startup handshake.
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d\r\n", p1x_, p1y_, p2x_, p2y_);
        MTRC_LOGF("OP (output P1/P2)");
        queueOutput(buf);
    } else if (mnemonic == "OA") {
        // Output Actual position: x,y,pen-status (0=up, 1=down).
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d\r\n", penX_, penY_, penDown_ ? 1 : 0);
        MTRC_LOGF("OA (output actual position)");
        queueOutput(buf);
    } else if (mnemonic == "OC") {
        // Output Commanded position -- same x,y,pen format as OA. Real
        // plotters distinguish "commanded" (where the last PA/PR/PU/PD
        // told the pen to go) from "actual" (where the carriage/motors
        // have physically settled to, accounting for acceleration/lag).
        // We don't simulate motor lag, so for us these are the same value.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d\r\n", penX_, penY_, penDown_ ? 1 : 0);
        MTRC_LOGF("OC (output commanded position)");
        queueOutput(buf);
    } else if (mnemonic == "OE") {
        // Output Error: the last error (1 = instruction not recognized),
        // 0 = none; reading it clears it and OS bit 5
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d\r\n", error_);
        MTRC_LOGF("OE (output error) -> %d", error_);
        error_ = 0;
        queueOutput(buf);
    } else if (mnemonic == "OI") {
        // Output Identification -- always "7470A" on a real 7470A,
        // regardless of interface (matches the SDI device-id string too).
        MTRC_LOGF("OI (output identification)");
        queueOutput("7470A\r\n");
    } else if (mnemonic == "OF") {
        // Output Factors -- plotter units per millimeter (X,Y). Fixed at
        // the real 7470A/7475A's standard resolution (40 units/mm, i.e.
        // 0.025mm per unit) -- not something we model differently.
        MTRC_LOGF("OF (output scaling factors)");
        queueOutput("40,40\r\n");
    } else if (mnemonic == "OO") {
        // Output Options -- 8 comma-separated flags for optional features.
        // Matches the manual's own real-hardware example exactly: arcs/
        // circles = 0 (an RS-232-C-only option, not applicable to our
        // HP-IL v1 anyway), pen select = 1 (we do support SP).
        MTRC_LOGF("OO (output options)");
        queueOutput("0,1,0,0,1,0,0,0\r\n");
    } else if (mnemonic == "OS") {
        // Output Status -- an 8-bit status byte. We only meaningfully
        // track bit 0 (pen down) and bit 3 (initialized, one-shot --
        // cleared the moment it's read, matching the spec); bit 4 (ready
        // for data) is always set since we have no real motors/pinch
        // wheels to wait on, and bit 5 (error) stays clear since OE
        // always reports no error. Power-on value is 24 (8+16), matching
        // the manual.
        int status = 16;
        if (penDown_) status |= 1;
        if (p1p2Changed_) { status |= 2; p1p2Changed_ = false; }
        if (pointReady_) status |= 4;
        if (error_ != 0) status |= 32;
        if (everInitialized_) {
            status |= 8;
            everInitialized_ = false;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d\r\n", status);
        MTRC_LOGF("OS (output status) -> %d", status);
        queueOutput(buf);
    } else if (mnemonic == "OW") {
        // Output Window -- the current IW clip rectangle (lower-left,
        // upper-right), same 4-integer format as OP.
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d\r\n", iw1x_, iw1y_, iw2x_, iw2y_);
        MTRC_LOGF("OW (output window)");
        queueOutput(buf);
    } else if (mnemonic == "TL") {
        // Tick Length: tp[,tn] as percentages of the P1-P2 span. tn
        // defaults to tp if omitted; bare "TL" resets both to the HP-GL
        // default (0.5%).
        tickLenPos_ = params.empty() ? 0.5 : params[0];
        tickLenNeg_ = params.size() > 1 ? params[1] : tickLenPos_;
        MTRC_LOGF("TL pos=%.3f%% neg=%.3f%%", tickLenPos_, tickLenNeg_);
    } else if (mnemonic == "XT") {
        MTRC_LOGF("XT (x-axis tick)");
        drawTick(/*vertical=*/true);
    } else if (mnemonic == "YT") {
        MTRC_LOGF("YT (y-axis tick)");
        drawTick(/*vertical=*/false);
    } else if (mnemonic == "IW") {
        // Input Window: a clip rectangle for every subsequently drawn
        // line (PD, ticks, label strokes) -- bare "IW" resets it to the
        // full P1-P2 area (i.e. no extra clipping).
        if (params.size() < 4) {
            iw1x_ = 0; iw1y_ = 0; iw2x_ = kMaxX; iw2y_ = kMaxY;   // (the whole area, as after IN)
            MTRC_LOGF("IW reset to P1-P2");
        } else {
            iw1x_ = static_cast<std::int16_t>(params[0]);
            iw1y_ = static_cast<std::int16_t>(params[1]);
            iw2x_ = static_cast<std::int16_t>(params[2]);
            iw2y_ = static_cast<std::int16_t>(params[3]);
            if (bTrace) {
                MLOGF("IW (%d,%d)-(%d,%d)", iw1x_, iw1y_, iw2x_, iw2y_);
            }
        }
    } else if (mnemonic == "SR") {
        // Character size: width%,height% of the P1-P2 span. Bare "SR"
        // resets to the HP-GL default (0.75%, 1.5%).
        const double wPct = params.empty() ? 0.75 : params[0];
        const double hPct = params.size() > 1 ? params[1] : wPct;
        charRelW_ = wPct;
        charRelH_ = hPct;
        charRelative_ = true;
        applyRelativeSize();
        if (bTrace) {
            MLOGF("SR w=%.3f%% h=%.3f%% (%.1f x %.1f units)",
                 wPct, hPct, charWidthUnits_, charHeightUnits_);
        }
    } else if (mnemonic == "SI") {
        // Absolute Character Size: width,height in centimeters, converted
        // to plotter units via the fixed 40 units/mm scale (see OF) --
        // NOT via the P1-P2 percentage SR uses, so character size stays
        // constant regardless of P1/P2. Bare "SI" defaults to 0.19cm/
        // 0.27cm, matching SR's own no-param default size at standard
        // P1/P2 (per the manual). Negative values mirror the label for
        // free -- glyphToPlotter()'s scaling already flips correctly for a
        // negative charWidthUnits_/charHeightUnits_.
        constexpr double kUnitsPerCm = 400.0;  // 10 mm/cm * 40 units/mm
        const double wCm = params.empty() ? 0.19 : params[0];
        const double hCm = params.size() > 1 ? params[1] : 0.27;
        charWidthUnits_  = wCm * kUnitsPerCm;
        charHeightUnits_ = hCm * kUnitsPerCm;
        charRelative_ = false;               // (a new P1/P2 leaves it alone)
        if (bTrace) {
            MLOGF("SI w=%.3fcm h=%.3fcm (%.1f x %.1f units)",
                 wCm, hCm, charWidthUnits_, charHeightUnits_);
        }
    } else if (mnemonic == "SL") {
        // Character slant: a shear factor (dx per unit y), not an angle in
        // degrees -- matches how oo7470A.rx's PxCSA is used directly.
        charSlant_ = params.empty() ? 0.0 : params[0];
        MTRC_LOGF("SL slant=%.3f", charSlant_);
    } else if (mnemonic == "DI") {
        // Label direction: a (run,rise) vector: bare "DI" resets to the
        // default (horizontal, i.e. (1,0)).
        if (params.empty()) {
            dirCos_ = 1.0; dirSin_ = 0.0;
        } else {
            const double run = params[0];
            const double rise = params.size() > 1 ? params[1] : 0.0;
            const double mag = std::sqrt(run * run + rise * rise);
            if (mag > 1e-9) { dirCos_ = run / mag; dirSin_ = rise / mag; }
        }
        // DI also resets the CR "home" position to the current pen
        // position (oo7470A.rx's newCRP), same as a real PA/PU/PD/PR move.
        crPosX_ = penX_; crPosY_ = penY_;
        MTRC_LOGF("DI cos=%.3f sin=%.3f", dirCos_, dirSin_);
    } else if (mnemonic == "CP") {
        // Character Plot: dCols spaces, dRows lines (bare CP: CP0,-1). The
        // lines move the CR point and the pen goes there (the start of
        // that line); the spaces are kept for the next label, counted from
        // the CR point (as the 7470A / emu7470: "LBSALES $" "CP-9,-1"
        // "LB(THOUSANDS)" starts the second label 9 spaces left of where
        // the first line began, not of where it ended)
        if (params.size() == 1) { error_ = 2; return; }     // (one number: wrong)
        const double dCols = params.empty() ? 0.0 : params[0];
        const double dRows = params.empty() ? -1.0 : params[1];
        const double sp = 1.5 * charWidthUnits_, ln = 2.0 * charHeightUnits_;
        cpOffX_ = dCols * sp * dirCos_;
        cpOffY_ = dCols * sp * dirSin_;
        crPosX_ -= dRows * ln * dirSin_;
        crPosY_ += dRows * ln * dirCos_;
        penX_ = clamp16(std::floor(crPosX_ + 0.5));          // (pen up -- nothing drawn)
        penY_ = clamp16(std::floor(crPosY_ + 0.5));
        if (onMove_ && !penDown_) onMove_(penX_, penY_);
        if (bTrace) MLOGF("CP %.2f,%.2f -> pen (%d,%d)", dCols, dRows, penX_, penY_);
    } else if (mnemonic == "CI") {
        // Circle: radius[,chord angle] around the pen -- the pen goes up
        // to the start (0 degrees; a negative radius: 180), down for the
        // circle, up again and back to the centre, then as it was
        if (params.empty()) return;
        const double r = params[0];
        const double chord = params.size() > 1 ? params[1] : 5.0;
        double fx, fy;
        unitFactors(fx, fy);
        const std::int16_t cx = penX_, cy = penY_;
        const bool wasDown = penDown_;
        MTRC_LOGF("CI r=%g chord=%g", r, chord);
        penDown_ = false;
        doMove(clamp16(std::floor(cx + r * fx + 0.5)), cy);
        penDown_ = true;
        arc(cx, cy, r, 0.0, 360.0, chord);
        penDown_ = false;
        doMove(cx, cy);
        penDown_ = wasDown;
    } else if (mnemonic == "AA" || mnemonic == "AR") {
        // Arc: centre x,y (AA absolute, AR from the pen), angle (degrees,
        // positive = counter-clockwise)[,chord angle] -- from the pen,
        // drawn if the pen is down; the pen ends at the arc's end
        if (params.size() < 3) return;
        double fx, fy;
        unitFactors(fx, fy);
        double cx, cy;
        if (mnemonic == "AR") {
            cx = penX_ + params[0] * fx;
            cy = penY_ + params[1] * fy;
        } else if (scaling_) {
            cx = p1x_ + (params[0] - scX1_) * fx;
            cy = p1y_ + (params[1] - scY1_) * fy;
        } else {
            cx = params[0];
            cy = params[1];
        }
        double vx, vy;
        startVector(cx, cy, vx, vy);
        MTRC_LOGF("%s centre (%.0f,%.0f) angle=%g", mnemonic.c_str(), cx, cy, params[2]);
        arc(cx, cy, vx, vy, params[2], params.size() > 3 ? params[3] : 5.0);
    } else if (mnemonic == "IP") {
        // Input P1/P2: the scaling points, in plotter units. Bare "IP":
        // the defaults; two numbers: P1 moves there and P2 with it (same
        // size). User units (SC) follow: they map onto the new P1..P2.
        if (params.size() != 0 && params.size() != 2 && params.size() != 4) {
            error_ = 2;                      // (wrong number of parameters: ignored)
            return;
        }
        if (params.empty()) {
            p1x_ = kP1X; p1y_ = kP1Y; p2x_ = kP2X; p2y_ = kP2Y;
        } else if (params.size() >= 2) {
            const int w = p2x_ - p1x_, h = p2y_ - p1y_;
            p1x_ = clamp16(params[0]);
            p1y_ = clamp16(params[1]);
            if (params.size() >= 4) {
                p2x_ = clamp16(params[2]);
                p2y_ = clamp16(params[3]);
            } else {
                p2x_ = clamp16(p1x_ + w);
                p2y_ = clamp16(p1y_ + h);
            }
        }
        p1p2Changed_ = true;
        applyRelativeSize();                 // (SR's size follows P1/P2)
        if (bTrace) MLOGF("IP P1=(%d,%d) P2=(%d,%d)", p1x_, p1y_, p2x_, p2y_);
    } else if (mnemonic == "SC") {
        // Scale: user units xmin,xmax,ymin,ymax -- xmin/ymin at P1, xmax/
        // ymax at P2 -- for PA/PR/PU/PD from now on. Bare "SC" (or a range
        // of zero): back to plotter units.
        if (params.size() != 0 && params.size() != 4) {
            error_ = 2;                      // (wrong number of parameters: ignored)
            return;
        }
        if (params.size() == 4 && params[1] != params[0] && params[3] != params[2]) {
            scX1_ = params[0]; scX2_ = params[1];
            scY1_ = params[2]; scY2_ = params[3];
            scaling_ = true;
            if (bTrace) MLOGF("SC x %g..%g, y %g..%g", scX1_, scX2_, scY1_, scY2_);
        } else {
            scaling_ = false;
            MTRC_LOGF("SC off (plotter units)");
        }
    } else if (mnemonic == "DF") {
        // Default: character size/slant/direction, tick length, label
        // terminator, absolute plotting, user units off -- pen position,
        // P1/P2, IW and the plot stay
        reset_defaults();
        coordMode_ = CoordMode::Absolute;
        scaling_ = false;
        lineType_ = -1;
        patternPct_ = 4.0;
        symbol_ = '\0';
        MTRC_LOGF("DF (defaults)");
    } else if (mnemonic == "DR") {
        // Relative direction: run,rise in % of P1..P2's width/height (the
        // label slants with P1/P2's shape); bare DR: horizontal
        const double run = params.empty() ? 1.0 : params[0] / 100.0 * (p2x_ - p1x_);
        const double rise = params.size() > 1 ? params[1] / 100.0 * (p2y_ - p1y_) : 0.0;
        const double mag = std::sqrt(run * run + rise * rise);
        if (mag > 1e-9) { dirCos_ = run / mag; dirSin_ = rise / mag; }
        crPosX_ = penX_; crPosY_ = penY_;
        MTRC_LOGF("DR cos=%.3f sin=%.3f", dirCos_, dirSin_);
    } else if (mnemonic == "VS" || mnemonic == "IM") {
        // Pen speed / input mask (which errors light the error LED, ask
        // for service): nothing to do here
        MTRC_LOGF("%s (accepted, nothing to do)", mnemonic.c_str());
    } else if (mnemonic == "CS" || mnemonic == "CA") {
        // Character set 0..4 (standard / alternate); another number: error
        // 3, ignored. Bare: set 0
        const int set = params.empty() ? 0 : static_cast<int>(params[0]);
        if (set < 0 || set > 4) {
            error_ = 3;
        } else if (mnemonic == "CS") {
            charSet_ = set;
        } else {
            altSet_ = set;
        }
        MTRC_LOGF("%s %d", mnemonic.c_str(), set);
    } else if (mnemonic == "SS" || mnemonic == "SA") {
        useAltSet_ = mnemonic == "SA";       // labels from now on in that set
    } else if (mnemonic == "LT") {
        // Line type 0..6[,pattern length in % of the P1..P2 diagonal,
        // default 4]; bare LT: solid
        if (params.empty() || params[0] < 0 || params[0] > 6) {
            lineType_ = -1;
        } else {
            lineType_ = static_cast<int>(params[0]);
            if (params.size() > 1 && params[1] > 0) patternPct_ = params[1];
        }
        patPos_ = 0.0;
        MTRC_LOGF("LT %d (%.1f%%)", lineType_, patternPct_);
    } else if (mnemonic == "DP") {
        // Digitize Point: wait for one (see digitize()); a point not yet
        // read is dropped
        digitizing_ = true;
        pointReady_ = false;
        LOGF("\r\n * %s: DP -- waiting for a point (the Plotter view: touch it, arrows, ENTER)", name());
    } else if (mnemonic == "DC") {
        digitizing_ = false;                 // Digitize Clear
        MTRC_LOGF("DC (digitize clear)");
    } else if (mnemonic == "OD") {
        // Output Digitized point: x,y,pen -- the last one; clears OS bit 2
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d\r\n", digX_, digY_, digPen_ ? 1 : 0);
        pointReady_ = false;
        MTRC_LOGF("OD (output digitized point)");
        queueOutput(buf);
    } else if (mnemonic == "UC") {
        // User Defined Character -- explicitly a NOP on a real 7470A with
        // an HP-IL interface (verbatim from the manual: "It is not
        // included in the instruction set of the 7470 plotter with an
        // HP-IL interface... treated as a NOP"). Only the HP-IB/RS-232-C
        // interface variants actually draw custom characters with it.
        // Recognized and silently ignored here rather than falling
        // through to "unrecognized command" -- this is correct,
        // documented behavior for our interface, not a missing feature.
        MTRC_LOGF("UC (NOP on HP-IL interface, per manual)");
    } else {
        // Not implemented (see plotter.h): ignored -- logged once since the
        // last IN, trace or not, so a program's missing pieces show up
        error_ = 1;                          // OE: instruction not recognized
        if (unsupported_.find(mnemonic + " ") == std::string::npos) {
            unsupported_ += mnemonic + " ";
            LOGF("\r\n * %s: HP-GL '%s' not supported -- ignored (\"%.40s\")", name(), mnemonic.c_str(), cmd.c_str());
        }
    }
}

void CPlotter::show(void) {
    CDevice::show();
    LOGF("\r\n\tpen:%u %s pos:(%d,%d) mode:%s segments:%zu parser:'%s' outQueue:%zu%s",
         currentPen_, penDown_ ? "DOWN" : "UP", penX_, penY_,
         coordMode_ == CoordMode::Absolute ? "ABS" : "REL",
         segments_.size(), cmdBuf_.c_str(), outQueue_.size(),
         outEnd_ ? " (end)" : "");
    LOGF("\r\n\tIW:(%d,%d)-(%d,%d) charSize:(%.1f,%.1f) CRpoint:(%d,%d)",
         iw1x_, iw1y_, iw2x_, iw2y_, charWidthUnits_, charHeightUnits_,
         static_cast<int>(crPosX_), static_cast<int>(crPosY_));
    LOGF("\r\n\tP1:(%d,%d) P2:(%d,%d) SC:", p1x_, p1y_, p2x_, p2y_);
    if (scaling_) LOGF("%g,%g,%g,%g", scX1_, scX2_, scY1_, scY2_);
    else LOGF("off");
    if (digitizing_) LOGF(" digitizing");
    if (!unsupported_.empty()) LOGF(" not supported: %s", unsupported_.c_str());
}
