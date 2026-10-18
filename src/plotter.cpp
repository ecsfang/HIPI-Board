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
    // size), converted to plotter units up front so drawLabelChar() never
    // has to redo the percentage math per character.
    charWidthUnits_  = 0.75 / 100.0 * (p2x_ - p1x_);
    charHeightUnits_ = 1.5  / 100.0 * (p2y_ - p1y_);
    charSlant_ = 0.0;
    dirCos_ = 1.0; dirSin_ = 0.0;
    labelTerminator_ = 0x03;  // ETX -- see plotter.h's note on DT/DF
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

    // IW defaults to the full P1-P2 area -- i.e. no extra clipping beyond
    // the hard-clip limits already implied by plotter coordinates.
    iw1x_ = kP1X; iw1y_ = kP1Y; iw2x_ = kP2X; iw2y_ = kP2Y;

    everInitialized_ = true;  // OS's power-on status bit, see plotter.h
    p1p2Changed_ = false;
    digitizing_ = false;
    pointReady_ = false;
    unsupported_.clear();

    reset_defaults();
    charPosX_ = penX_; charPosY_ = penY_;
    crPosX_ = penX_; crPosY_ = penY_;

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

    segments_.push_back({ix0, iy0, ix1, iy1, currentPen_});
    if (bTrace) {
        MLOGF("  draw (%d,%d) -> (%d,%d) pen=%u", ix0, iy0, ix1, iy1, currentPen_);
    }
    if (onDraw_) onDraw_(ix0, iy0, ix1, iy1, currentPen_);
}

void CPlotter::doMove(std::int16_t x, std::int16_t y) {
    if (penDown_) {
        MTRC_LOGF("  (pen down move)");
        emitSegment(penX_, penY_, x, y);
    } else {
        MTRC_LOGF("  move -> (%d,%d)", x, y);
        if (onMove_) onMove_(x, y);
    }
    penX_ = x;
    penY_ = y;
    // Keep the label "text cursor" AND its CR "home" position in sync
    // with every real pen move -- CP (see execute()) is the only thing
    // that deliberately offsets charPosX_/Y_ away from the actual pen
    // position afterward, and it does NOT touch crPosX_/Y_ (matches
    // oo7470A.rx: PA/PU/PD/PR and DI reset the CR home point, CP doesn't).
    charPosX_ = x;
    charPosY_ = y;
    crPosX_ = x;
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
    }
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

void CPlotter::drawLabel(const std::string& text) {
    MTRC_LOGF("LB \"%s\"", text.c_str());
    for (char ch : text) {
        const unsigned char code = static_cast<unsigned char>(ch);
        if (code == 13) {
            // Carriage return: snap the text cursor back onto the same
            // "column" (position along the text direction) it started
            // at -- crPosX_/crPosY_, the CR home point -- while keeping
            // whatever offset perpendicular to that direction has built
            // up since then (e.g. from an embedded LF moving down a
            // row). This is a vector projection of (crPos - charPos)
            // onto the (unit) direction vector, ported directly from
            // oo7470A.rx's CR handling in its LB command. Without this
            // special case, CR would just fall through to drawLabelChar()
            // as an unrecognized glyph and merely advance one character
            // width instead of returning to the line's start column.
            const double a = (crPosX_ - charPosX_) * dirCos_ + (crPosY_ - charPosY_) * dirSin_;
            charPosX_ = static_cast<std::int16_t>(charPosX_ + a * dirCos_);
            charPosY_ = static_cast<std::int16_t>(charPosY_ + a * dirSin_);
            MTRC_LOGF("  CR -> textpos(%d,%d)", charPosX_, charPosY_);
            continue;
        }
        drawLabelChar(code);
    }
}

void CPlotter::drawLabelChar(unsigned char code) {
    // Build the full stroke list: the font table (hpgl_font.h) holds each
    // glyph's own strokes verbatim from oo7470A.rx's chagra. table, which
    // does NOT yet include the inter-character advance -- that's appended
    // here exactly like oo7470A.rx's LB handler does (",0, 48, 0", a
    // final pen-up move to x=48 in the glyph's own coordinate space),
    // except for control characters (code <= 32, e.g. space/BS/LF/VT)
    // whose own glyph data already fully encodes their specific advance.
    std::string strokes;
    const bool known = (code < 128) && (hpgl_font::kGlyphs[code] != nullptr);
    if (known) {
        strokes = hpgl_font::kGlyphs[code];
        if (code > 32) strokes += ",0, 48,  0";
    } else {
        strokes = "0, 48,  0";  // unrecognized glyph -- just advance like a space
    }

    const std::vector<double> nums = parseParams(strokes, 0);
    const std::int16_t originX = charPosX_, originY = charPosY_;
    std::int16_t prevX = originX, prevY = originY;
    std::int16_t lastX = originX, lastY = originY;
    bool first = true;

    for (std::size_t i = 0; i + 2 < nums.size(); i += 3) {
        const bool penDown = nums[i] != 0.0;
        double sx = charWidthUnits_  * nums[i + 1] / 32.0;
        double sy = charHeightUnits_ * nums[i + 2] / 32.0;
        sx += charSlant_ * sy;                            // SL: shear x by slant*y
        const double rx = sx * dirCos_ - sy * dirSin_;    // DI: rotate into plotter space
        const double ry = sx * dirSin_ + sy * dirCos_;
        const auto x = static_cast<std::int16_t>(originX + rx);
        const auto y = static_cast<std::int16_t>(originY + ry);

        if (!first && penDown) emitSegment(prevX, prevY, x, y);
        prevX = x; prevY = y;
        lastX = x; lastY = y;
        first = false;
    }

    // The glyph's final point (after the appended advance move) becomes
    // the next character's origin.
    charPosX_ = lastX;
    charPosY_ = lastY;
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
        // Output Error -- we don't track real plotter error conditions
        // yet, so this always reports "no error".
        MTRC_LOGF("OE (output error status)");
        queueOutput("0\r\n");
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
            iw1x_ = kP1X; iw1y_ = kP1Y; iw2x_ = kP2X; iw2y_ = kP2Y;   // (the default area, not a changed P1/P2)
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
        charWidthUnits_  = wPct / 100.0 * (p2x_ - p1x_);
        charHeightUnits_ = hPct / 100.0 * (p2y_ - p1y_);
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
        // free -- drawLabelChar()'s scaling already flips correctly for a
        // negative charWidthUnits_/charHeightUnits_.
        constexpr double kUnitsPerCm = 400.0;  // 10 mm/cm * 40 units/mm
        const double wCm = params.empty() ? 0.19 : params[0];
        const double hCm = params.size() > 1 ? params[1] : 0.27;
        charWidthUnits_  = wCm * kUnitsPerCm;
        charHeightUnits_ = hCm * kUnitsPerCm;
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
        // Character Plot: reposition the label "text cursor" by dCols
        // character-widths and dRows character-heights, along the current
        // text direction (DI), relative to the CURRENT pen position (not
        // accumulated from a previous CP) -- matches oo7470A.rx, where
        // each CP is computed fresh from the last real PA/PU/PD position.
        // Bare "CP" (no params) means "one line down, same column".
        const double dCols = params.empty() ? 0.0 : params[0];
        const double dRows = params.size() > 1 ? params[1] : (params.empty() ? -1.0 : 0.0);
        const double dx = dCols * charWidthUnits_;
        const double dy = dRows * charHeightUnits_;
        charPosX_ = static_cast<std::int16_t>(penX_ + dx * dirCos_ - dy * dirSin_);
        charPosY_ = static_cast<std::int16_t>(penY_ + dx * dirSin_ + dy * dirCos_);
        if (bTrace) {
            MLOGF("CP dCols=%.2f dRows=%.2f -> textpos(%d,%d)",
                 dCols, dRows, charPosX_, charPosY_);
        }
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
        if (bTrace) MLOGF("IP P1=(%d,%d) P2=(%d,%d)", p1x_, p1y_, p2x_, p2y_);
    } else if (mnemonic == "SC") {
        // Scale: user units xmin,xmax,ymin,ymax -- xmin/ymin at P1, xmax/
        // ymax at P2 -- for PA/PR/PU/PD from now on. Bare "SC" (or a range
        // of zero): back to plotter units.
        if (params.size() >= 4 && params[1] != params[0] && params[3] != params[2]) {
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
        MTRC_LOGF("DF (defaults)");
    } else if (mnemonic == "DP") {
        // Digitize Point: wait for one (see digitize()); a point not yet
        // read is dropped
        digitizing_ = true;
        pointReady_ = false;
        LOGF("\r\n * %s: DP -- waiting for a point (touch it in the Plotter view)", name());
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
    LOGF("\r\n\tIW:(%d,%d)-(%d,%d) charSize:(%.1f,%.1f) textpos:(%d,%d)",
         iw1x_, iw1y_, iw2x_, iw2y_, charWidthUnits_, charHeightUnits_,
         charPosX_, charPosY_);
    LOGF("\r\n\tP1:(%d,%d) P2:(%d,%d) SC:", p1x_, p1y_, p2x_, p2y_);
    if (scaling_) LOGF("%g,%g,%g,%g", scX1_, scX2_, scY1_, scY2_);
    else LOGF("off");
    if (digitizing_) LOGF(" digitizing");
    if (!unsupported_.empty()) LOGF(" not supported: %s", unsupported_.c_str());
}
