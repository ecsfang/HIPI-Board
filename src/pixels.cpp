#define MODULE "PIXEL"

#include "pixels.h"
#include "usb_serial.h"
#include "Effects/Comet.hpp"
#include "Effects/Marquee.hpp"
#include "Effects/Stars.hpp"
#include "Effects/Bounce.hpp"
#include "Effects/Particles.hpp"
#include "Effects/Fade.hpp"
#include "pico/time.h"
#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <string>


// ─── RGB332 <-> RGB888 ─────────────────────────────────────────────────────
// bits [7:5]=R (3 bits, 0-7), [4:2]=G (3 bits, 0-7), [1:0]=B (2 bits, 0-3).
// Scaled linearly up to the full 0-255 range per channel (not just left-
// shifted, which would leave the brightest representable value short of
// actual 255 -- e.g. a naive R<<5 tops out at 224, not 255).
static void rgb332ToRgb888(std::uint8_t colorByte, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    const std::uint8_t r3 = (colorByte >> 5) & 0x07;
    const std::uint8_t g3 = (colorByte >> 2) & 0x07;
    const std::uint8_t b2 = colorByte & 0x03;
    r = static_cast<std::uint8_t>((r3 * 255) / 7);
    g = static_cast<std::uint8_t>((g3 * 255) / 7);
    b = static_cast<std::uint8_t>((b2 * 255) / 3);
}

static PicoLed::Color rgb332Color(double v) {
    std::uint8_t r, g, b;
    const int c = static_cast<int>(v);
    rgb332ToRgb888(static_cast<std::uint8_t>(c < 0 ? 0 : c > 255 ? 255 : c), r, g, b);
    return PicoLed::RGB(r, g, b);
}

// A pixel's colour as it's in the strip's buffer. PicoLedController
// declares getPixelColor() but the library never defines it; its target
// (protected) has a working one -- reached through a derived class.
namespace {
struct ControllerAccess : PicoLed::PicoLedController {
    static PicoLed::Color pixel(PicoLed::PicoLedController& c, uint index) {
        return (c.*(&ControllerAccess::target))->getPixelColor(index);
    }
};
}  // namespace

// ─── Rainbow effect ─────────────────────────────────────────────────────────
// Not one of PicoLED's own effects: the whole range in rainbow colours,
// rolling along. speed: hue steps (of 256) a second; step: hue difference
// between neighbouring pixels.
namespace {
class RainbowEffect : public PicoLed::PicoLedEffect {
public:
    RainbowEffect(PicoLed::PicoLedController& c, double speed, double step)
        : PicoLed::PicoLedEffect(c), speed_(speed), step_(step) {}
protected:
    bool update(uint32_t timeGone, uint32_t) override {
        hue_ += speed_ * static_cast<double>(timeGone) / 1000.0;
        while (hue_ >= 256.0) hue_ -= 256.0;
        controller.fillRainbow(static_cast<std::uint8_t>(hue_), static_cast<std::uint8_t>(step_));
        return true;
    }
private:
    double speed_, step_, hue_ = 0.0;
};
}  // namespace

// ─── CPixelStrip ────────────────────────────────────────────────────────────

CPixelStrip::CPixelStrip(uint pin, uint count, PIO pio, uint sm)
    : strip_(PicoLed::addLeds<PicoLed::WS2812B>(pio, sm, pin, count, PIXEL_COLOR_ORDER)),
      count_(count),
      active_(std::min<uint32_t>(PIXEL_COUNT_INITIAL, count)),
      lastR_(count, 0), lastG_(count, 0), lastB_(count, 0),
      currentR_(count, 0), currentG_(count, 0), currentB_(count, 0) {
    strip_.setBrightness(255);  // full brightness by default -- the "S" ASCII
                                 // command scales this down later if asked
    strip_.clear();
    strip_.show();
}

void CPixelStrip::_remember(uint32_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (index < lastR_.size()) {
        lastR_[index] = r;
        lastG_[index] = g;
        lastB_[index] = b;
    }
}

void CPixelStrip::_setCurrent(uint32_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (index < currentR_.size()) {
        currentR_[index] = r;
        currentG_[index] = g;
        currentB_[index] = b;
    }
}

void CPixelStrip::_traceOutOfRange(uint32_t index, const char* what) const {
    MDBG_LOGF("%s: index %lu out of range (strip has %lu pixel(s)) -- ignored",
             what, static_cast<unsigned long>(index), static_cast<unsigned long>(count_));
}

void CPixelStrip::setPixelRGB332(uint32_t index, std::uint8_t colorByte) {
    if (index >= count()) { _traceOutOfRange(index, "setPixelRGB332"); return; }
    std::uint8_t r, g, b;
    rgb332ToRgb888(colorByte, r, g, b);
    setPixelRGB(index, r, g, b);
}

void CPixelStrip::setPixelRGB(uint32_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (index >= count()) { _traceOutOfRange(index, "setPixelRGB"); return; }
    strip_.setPixelColor(index, PicoLed::RGB(r, g, b));
    _remember(index, r, g, b);
    _setCurrent(index, r, g, b);
}

void CPixelStrip::getPixelColor(uint32_t index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
    if (index >= count_) { r = g = b = 0; return; }
    r = currentR_[index];
    g = currentG_[index];
    b = currentB_[index];
}

void CPixelStrip::off(uint32_t index) {
    if (index >= count()) { _traceOutOfRange(index, "off"); return; }
    // Deliberately does NOT touch lastR_/lastG_/lastB_ -- on() below needs
    // them to still hold whatever color was showing before this call.
    strip_.setPixelColor(index, PicoLed::RGB(0, 0, 0));
    _setCurrent(index, 0, 0, 0);
}

void CPixelStrip::on(uint32_t index) {
    if (index >= count()) { _traceOutOfRange(index, "on"); return; }
    // White (0xFFFFFF) if this pixel's never actually had a color set --
    // lastR_/G_/B_ are zero-initialized at construction, which would
    // otherwise silently mean "on" does nothing the very first time.
    std::uint8_t r = lastR_[index], g = lastG_[index], b = lastB_[index];
    if (r == 0 && g == 0 && b == 0) { r = g = b = 255; }
    strip_.setPixelColor(index, PicoLed::RGB(r, g, b));
    _remember(index, r, g, b);
    _setCurrent(index, r, g, b);
}

void CPixelStrip::setBrightness(std::uint8_t pct) {
    const std::uint8_t clamped = pct > 100 ? 100 : pct;
    brightness_ = static_cast<std::uint8_t>((static_cast<uint16_t>(clamped) * 255) / 100);
}

void CPixelStrip::clear() {
    strip_.clear();
    // NOT clearing lastR_/G_/B_ here either -- "0C" then "0O" should bring
    // everything back exactly as it was, same reasoning as off() above.
    for (uint32_t i = 0; i < count_; ++i) _setCurrent(i, 0, 0, 0);
}

void CPixelStrip::show() {
    // Current limit (PIXEL_POWER_LIMIT_MA): the sum of all channel levels
    // times the brightness, against what the limit allows
    std::uint32_t sum = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        const PicoLed::Color c = ControllerAccess::pixel(strip_, i);
        sum += static_cast<std::uint32_t>(c.red) + c.green + c.blue;
    }
    constexpr std::uint32_t kLimit = static_cast<std::uint32_t>(PIXEL_POWER_LIMIT_MA) * 255u * 255u /
                                     PIXEL_MA_PER_CHANNEL;
    std::uint32_t level = brightness_;
    if (sum > 0 && sum * level > kLimit) {
        // In steps of 8, so an effect doesn't change it every frame
        level = std::max<std::uint32_t>(1, (kLimit / sum) & ~7u);
    }
    // (PicoLED logs every brightness change: only when it changes)
    if (level != applied_) {
        strip_.setBrightness(static_cast<std::uint8_t>(level));
        applied_ = static_cast<std::uint8_t>(level);
    }
    strip_.show();
}

void CPixelStrip::_blank(uint32_t first, uint32_t last) {
    for (uint32_t i = first; i <= last && i < count_; ++i) {
        strip_.setPixelColor(i, PicoLed::RGB(0, 0, 0));
        _setCurrent(i, 0, 0, 0);
    }
}

void CPixelStrip::setActiveCount(uint32_t n, bool notify) {
    n = std::max<uint32_t>(1, std::min(n, count_));
    if (n == active_) return;
    if (n < active_) {
        stopEffects(n, count_ - 1);         // effects reaching beyond the new end
        _blank(n, count_ - 1);
        show();
    }
    MTRC_LOGF("%lu pixel(s) connected", static_cast<unsigned long>(n));
    active_ = n;
    if (notify && countChanged_) countChanged_(n);
}

bool CPixelStrip::stopEffects(uint32_t first, uint32_t last) {
    bool any = false;
    for (auto it = effects_.begin(); it != effects_.end(); ) {
        if (it->first <= last && first <= it->last) {
            MTRC_LOGF("effect on %lu-%lu stopped", static_cast<unsigned long>(it->first + 1),
                      static_cast<unsigned long>(it->last + 1));
            _blank(it->first, it->last);
            it = effects_.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    return any;
}

bool CPixelStrip::startEffect(uint32_t first, uint32_t last, char type,
                              const std::vector<std::vector<double>>& params) {
    if (first > last || last >= active_) return false;
    const uint32_t n = last - first + 1;
    // Parameter k (single number) or its default, clamped to lo..hi
    auto num = [&](std::size_t k, double def, double lo, double hi) {
        double v = (k < params.size() && !params[k].empty()) ? params[k][0] : def;
        return v < lo ? lo : v > hi ? hi : v;
    };
    // Parameter k as a palette (RGB332 numbers), or the default colours
    auto palette = [&](std::size_t k, std::initializer_list<int> def) {
        std::vector<PicoLed::Color> pal;
        if (k < params.size() && !params[k].empty()) {
            for (double v : params[k]) pal.push_back(rgb332Color(v));
        } else {
            for (int v : def) pal.push_back(rgb332Color(v));
        }
        return pal;
    };

    if (type == 'F' && n < 3) {
        MLOGF("effect F (fire) needs at least 3 pixels -- ignored");
        return false;
    }
    stopEffects(first, last);                     // what ran there before
    // The effect sees only its own pixels (a VirtualStrip over them)
    PicoLed::PicoLedController part = strip_.slice(first, last + 1);
    std::unique_ptr<PicoLed::PicoLedEffect> fx;
    switch (type) {
    case 'C':   // colour : speed (pixels/s) : length : fade
        fx.reset(new PicoLed::Comet(part, palette(0, { 255 })[0], num(1, 8, 0.1, 500),
                                    num(2, 2, 0.5, n), num(3, 2, 0.05, 50)));
        break;
    case 'M':   // palette : length : speed : spacing
        fx.reset(new PicoLed::Marquee(part, palette(0, { 224, 28, 3 }), num(1, 2, 0.5, n),
                                      num(2, 4, -500, 500), num(3, 1, 0, n)));
        break;
    case 'S':   // palette : stars a second : fade
        fx.reset(new PicoLed::Stars(part, palette(0, { 255 }), num(1, 3, 0.1, 100), num(2, 1, 0.05, 50)));
        break;
    case 'B': { // balls : palette : gravity : fade
        auto* b = new PicoLed::Bounce(part, num(3, 3, 0.05, 50), num(2, 20, 1, 500));
        const auto pal = palette(1, { 224, 28, 3 });
        const int balls = static_cast<int>(num(0, 3, 1, 8));
        for (int i = 0; i < balls; ++i) b->addBall(pal[static_cast<std::size_t>(i) % pal.size()], 1.0);
        fx.reset(b);
        break;
    }
    case 'F': { // intensity : spread : cooling
        static const int kFire[][3] = { { 0, 0, 0 }, { 160, 0, 0 }, { 255, 80, 0 },
                                        { 255, 200, 0 }, { 255, 255, 160 } };
        std::vector<PicoLed::Color> pal;
        for (const auto& c : kFire) pal.push_back(PicoLed::RGB(c[0], c[1], c[2]));
        auto* f = new PicoLed::Particles(part, pal, num(1, 1, 0.1, 10), num(2, 1, 0.05, 20));
        f->addSource(0, num(0, 2, 0.1, 50));
        fx.reset(f);
        break;
    }
    case 'O':   // rate
        fx.reset(new PicoLed::Fade(part, PicoLed::RGB(0, 0, 0), num(0, 1, 0.05, 50)));
        break;
    case 'R':   // speed : step
        fx.reset(new RainbowEffect(part, num(0, 40, -1000, 1000), num(1, std::max(1.0, 256.0 / n), 0, 255)));
        break;
    default:
        MLOGF("unknown effect '%c' -- ignored", type);
        return false;
    }
    // Room for it: the oldest one goes
    if (effects_.size() >= PIXEL_MAX_EFFECTS) {
        _blank(effects_.front().first, effects_.front().last);
        effects_.erase(effects_.begin());
    }
    effects_.push_back(Effect{ first, last, std::move(fx) });
    MTRC_LOGF("effect %c on %lu-%lu started", type, static_cast<unsigned long>(first + 1),
              static_cast<unsigned long>(last + 1));
    nextFrameMs_ = 0;                             // first frame at once
    return true;
}

void CPixelStrip::poll() {
    if (effects_.empty()) return;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (static_cast<std::int32_t>(now - nextFrameMs_) < 0) return;
    nextFrameMs_ = now + PIXEL_FRAME_MS;
    bool changed = false;
    for (Effect& e : effects_) changed |= e.fx->animate();
    if (changed) show();
}

// The project's single, global pixel strip -- mirrors leds.cpp's own
// ledPower/ledStatus/etc. global instances.
CPixelStrip pixelStrip(PIXEL_PIN, PIXEL_COUNT_DEFAULT);

// Mirrors leds.cpp's own global `parser` (CLedParser) -- ilpixels.cpp's
// CHipiPixel::doListener()/clear() feed incoming HP-IL bytes into this.
CPixelParser pixelParser(pixelStrip);

// ─── CPixelParser ───────────────────────────────────────────────────────────

void CPixelParser::_reset() {
    _state = State::Idle;
    _selectAll = false;
    _selectFrom = _selectTo = -1;
    _paramVal = 0;
    _hasDigit = false;
    _runCount = _runStart = _runIndex = 0;
    _run332 = false;
    _runR = _runG = 0;
    _effectType = 0;
    _effectParams.clear();
}

// ─── Selections, effects ────────────────────────────────────────────────────

bool CPixelParser::_selectionRange(uint32_t& first, uint32_t& last) const {
    const uint32_t n = _strip.count();
    if (_selectAll || _selectFrom < 0) {          // "0", or nothing: all
        first = 0;
        last = n - 1;
        return true;
    }
    const int32_t from = std::max<int32_t>(1, _selectFrom);
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    if (to < from || static_cast<uint32_t>(from) > n) return false;
    first = static_cast<uint32_t>(from - 1);
    last = std::min<uint32_t>(static_cast<uint32_t>(to), n) - 1;
    return true;
}

void CPixelParser::_claimSelection() {
    // (no selection at all: the command does nothing, so nothing to stop)
    if (!_selectAll && _selectFrom < 0) return;
    uint32_t first, last;
    if (_selectionRange(first, last)) _strip.stopEffects(first, last);
}

void CPixelParser::_stopEffectsInSelection() {
    uint32_t first, last;
    if (!_selectionRange(first, last)) return;
    if (_strip.stopEffects(first, last)) _strip.show();
}

// The collected "E<t><params>": parameters are numbers, ':' between them
// and ',' between the colours of a palette (see pixels.h)
void CPixelParser::_startEffect() {
    std::vector<std::vector<double>> params(1);
    const char* p = _effectParams.c_str();
    while (*p != '\0') {
        if (*p == ':') { params.emplace_back(); ++p; continue; }
        if (*p == ',') { ++p; continue; }
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) { ++p; continue; }          // not a number: skip it
        params.back().push_back(v);
        p = end;
    }
    uint32_t first, last;
    if (!_selectionRange(first, last)) {
        MLOGF("effect %c: pixels %s aren't connected (%lu) -- ignored", _effectType,
              _selectionDesc().c_str(), static_cast<unsigned long>(_strip.count()));
        return;
    }
    MTRC_LOGF("effect %c on %s, parameters \"%s\"", _effectType, _selectionDesc().c_str(),
              _effectParams.c_str());
    if (_strip.startEffect(first, last, _effectType, params)) _strip.show();
}

std::string CPixelParser::_selectionDesc() const {
    if (_selectAll) return "all";
    if (_selectFrom < 0) return "(none)";
    if (_selectTo < 0 || _selectTo == _selectFrom) {
        return std::to_string(_selectFrom);
    }
    return std::to_string(_selectFrom) + "-" + std::to_string(_selectTo);
}

// Short, human-readable name for a parser state -- used only by feed()'s
// own byte-level bExtTrace line below.
const char* CPixelParser::_stateName(State s) {
    switch (s) {
        case State::Idle:            return "Idle";
        case State::AwaitBrightness: return "AwaitBrightness";
        case State::AwaitCount:      return "AwaitCount";
        case State::AwaitEffectType: return "AwaitEffectType";
        case State::AwaitEffectParams: return "AwaitEffectParams";
        case State::AwaitX:          return "AwaitX";
        case State::AwaitY1:         return "AwaitY1";
        case State::AwaitY2:         return "AwaitY2";
        case State::AwaitY3:         return "AwaitY3";
        case State::AwaitRunCount:   return "AwaitRunCount";
        case State::AwaitRunStart:   return "AwaitRunStart";
        case State::AwaitRun332:     return "AwaitRun332";
        case State::AwaitRunR:       return "AwaitRunR";
        case State::AwaitRunG:       return "AwaitRunG";
        case State::AwaitRunB:       return "AwaitRunB";
    }
    return "?";
}

void CPixelParser::feed(std::uint8_t c) {
    // Every single byte, however it ends up being interpreted -- the most
    // detailed level available, deliberately gated to bExtTrace only
    // (this fires once per byte, so it's far too noisy for normal bTrace
    // use). Printable range shown as a character too, since raw ASCII
    // command letters/digits/selectors are common in this protocol.
    if (bExtTrace) {
        if (c >= 0x20 && c < 0x7F)
            MLOGF("feed: 0x%02X ('%c') in state %s", c, c, _stateName(_state));
        else
            MLOGF("feed: 0x%02X in state %s", c, _stateName(_state));
    }

    switch (_state) {

        // ── Binary run modes: every byte here is a literal value, never
        // interpreted as a command/digit/separator regardless of what it
        // looks like as ASCII. ──────────────────────────────────────────
        case State::AwaitRunCount:
            _runCount = c;
            _state = State::AwaitRunStart;
            return;

        case State::AwaitRunStart:
            _runStart = c;          // pixel number: 1 = first; 0 = all (pattern repeated)
            _runIndex = 0;
            _runPattern.clear();
            if (_runCount == 0) {
                MTRC_LOGF("run(%s): count=0, start=%u -- nothing to do",
                                 _run332 ? "RGB332" : "RGB24", _runStart);
                _reset();
                return;
            }  // count=0: nothing to apply
            MTRC_LOGF("run(%s): count=%u start=%u",
                             _run332 ? "RGB332" : "RGB24", _runCount, _runStart);
            // The pixels it sets: no effect there any more
            if (_runStart == 0) _strip.stopAllEffects();
            else _strip.stopEffects(_runStart - 1u, _runStart - 1u + _runCount - 1u);
            _state = _run332 ? State::AwaitRun332 : State::AwaitRunR;
            return;

        case State::AwaitRun332:
            _runPixel(c, 0, 0, true);
            if (++_runIndex >= _runCount) {
                _runFinish();
                _strip.show();
                MTRC_LOGF("run(RGB332) complete: %u pixel(s) from %u",
                                 _runCount, _runStart);
                _reset();
            }
            return;

        case State::AwaitRunR:
            _runR = c;
            _state = State::AwaitRunG;
            return;

        case State::AwaitRunG:
            _runG = c;
            _state = State::AwaitRunB;
            return;

        case State::AwaitRunB:
            _runPixel(_runR, _runG, c, false);
            if (++_runIndex >= _runCount) {
                _runFinish();
                _strip.show();
                MTRC_LOGF("run(RGB24) complete: %u pixel(s) from %u",
                                 _runCount, _runStart);
                _reset();
            } else {
                _state = State::AwaitRunR;
            }
            return;

        // ── 'X'/'Y' ASCII commands' own raw color byte(s) -- also literal,
        // never interpreted as digits/separators. ──────────────────────
        case State::AwaitX:
            MTRC_LOGF("set %s = RGB332 0x%02X", _selectionDesc().c_str(), c);
            _claimSelection();
            _applyToSelection332(c);
            _strip.show();
            _reset();
            return;

        case State::AwaitY1:
            _runR = c;
            _state = State::AwaitY2;
            return;

        case State::AwaitY2:
            _runG = c;
            _state = State::AwaitY3;
            return;

        case State::AwaitY3:
            if (bTrace) {
                MLOGF("set %s = RGB(%u,%u,%u)",
                     _selectionDesc().c_str(), _runR, _runG, c);
            }
            _claimSelection();
            _applyToSelection(_runR, _runG, c);
            _strip.show();
            _reset();
            return;

        // ── Pixel count digits ('N' seen) ────────────────────────────────
        case State::AwaitCount:
            if (c >= '0' && c <= '9') {
                _paramVal = std::min<int32_t>(_paramVal * 10 + (int32_t)(c - '0'), 1000);
                _hasDigit = true;
                return;
            }
            if (_hasDigit) _strip.setActiveCount(static_cast<uint32_t>(_paramVal));
            _reset();
            feed(c);                       // the byte that ended it, from Idle
            return;

        // ── Effect ('E' seen): its letter, then its parameters ───────────
        case State::AwaitEffectType:
            if (c == ' ' || c == '\n' || c == '\r' || c == ';' || c == '\0') {
                MTRC_LOGF("effects stopped: %s", _selectionDesc().c_str());
                _stopEffectsInSelection();     // "E" alone: stop
                _reset();
                return;
            }
            _effectType = static_cast<char>((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
            _effectParams.clear();
            _state = State::AwaitEffectParams;
            return;

        case State::AwaitEffectParams:
            if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == ':' || c == '-') {
                if (_effectParams.size() < 64) _effectParams.push_back(static_cast<char>(c));
                return;
            }
            _startEffect();
            _reset();
            feed(c);                       // the byte that ended it, from Idle
            return;

        // ── Brightness digits ('S' seen) ─────────────────────────────────
        case State::AwaitBrightness:
            if (c >= '0' && c <= '9') {
                _paramVal = _paramVal * 10 + (int32_t)(c - '0');
                _hasDigit = true;
                return;
            }
            // Any non-digit ends the brightness value -- apply, then
            // re-process this byte from Idle (mirrors CLedParser's own
            // "finalize, then re-feed" pattern for exactly this situation).
            {
                const int32_t pct = _hasDigit ? std::min<int32_t>(_paramVal, 100) : 0;
                MTRC_LOGF("brightness -> %ld%%", static_cast<long>(pct));
                _strip.setBrightness(static_cast<std::uint8_t>(pct));
            }
            _strip.show();
            _reset();
            feed(c);
            return;

        // ── Idle: pixel selection digits, '-' range separator, or a
        // command letter. ────────────────────────────────────────────────
        case State::Idle:
            if (c == ' ' || c == '\n' || c == '\r' || c == ';' || c == '\0') {
                _reset();  // no active command -- nothing to finalize
                return;
            }
            // A '0' on its own means all pixels -- but not inside a number
            // ("10", "20", "100" are pixel numbers)
            if (c == '0' && _selectFrom < 0 && !_hasDigit) {
                _selectAll = true;
                return;
            }
            if (c >= '0' && c <= '9') {
                _paramVal = _paramVal * 10 + (int32_t)(c - '0');
                _hasDigit = true;
                return;
            }
            if (c == '-' && _hasDigit && _selectTo < 0) {
                _selectFrom = _paramVal;
                _paramVal = 0;
                _hasDigit = false;
                return;
            }
            // A pixel-selector number (single or range start) finished
            // accumulating right before this command letter.
            if (_hasDigit) {
                if (_selectFrom < 0) _selectFrom = _paramVal;
                else                 _selectTo   = _paramVal;
                _paramVal = 0;
                _hasDigit = false;
            }
            switch (c) {
                case 'C':
                    MTRC_LOGF("off: %s", _selectionDesc().c_str());
                    _claimSelection();
                    _onOff(); _strip.show(); _reset(); return;
                case 'O':
                    MTRC_LOGF("on: %s", _selectionDesc().c_str());
                    _claimSelection();
                    _onOn();  _strip.show(); _reset(); return;
                case 'N': _paramVal = 0; _hasDigit = false; _state = State::AwaitCount; return;
                case 'E': _state = State::AwaitEffectType; return;
                case 'S': _state = State::AwaitBrightness; return;
                case 'X': _state = State::AwaitX; return;
                case 'Y': _state = State::AwaitY1; return;
                case '#': _run332 = true;  _state = State::AwaitRunCount; return;
                case '@': _run332 = false; _state = State::AwaitRunCount; return;
                default:
                    // Unrecognised byte with no active command -- ignore
                    // silently (matches CLedParser's own handling of
                    // stray/non-protocol bytes), but still worth a note
                    // at the extended trace level since it usually means
                    // either a framing bug somewhere upstream or garbage
                    // on the line -- bTrace stays quiet about it to match
                    // CLedParser's own convention of not treating this as
                    // a normal, reportable event.
                    if (bExtTrace) {
                        if (c >= 0x20 && c < 0x7F)
                            MLOGF("ignored stray byte 0x%02X ('%c') in Idle state", c, c);
                        else
                            MLOGF("ignored stray byte 0x%02X in Idle state", c);
                    }
                    return;
            }
    }
}

void CPixelParser::flush() {
    // Only an ASCII group can be meaningfully finalized here -- see this
    // method's own header comment (pixels.h) for why an interrupted binary
    // run is left pending instead.
    if (_state == State::Idle) { _reset(); return; }
    if (_state == State::AwaitBrightness) {
        const int32_t pct = _hasDigit ? std::min<int32_t>(_paramVal, 100) : 0;
        MTRC_LOGF("brightness -> %ld%% (at flush)", static_cast<long>(pct));
        _strip.setBrightness(static_cast<std::uint8_t>(pct));
        _strip.show();
    } else if (_state == State::AwaitCount) {
        if (_hasDigit) _strip.setActiveCount(static_cast<uint32_t>(_paramVal));
    } else if (_state == State::AwaitEffectType) {
        _stopEffectsInSelection();
    } else if (_state == State::AwaitEffectParams) {
        _startEffect();
    } else {
        // Any other pending state at flush() means the transmission ended
        // mid-command -- e.g. '#'/'@' cut off before its run finished, or
        // 'X'/'Y' cut off before its color byte(s) arrived. Nothing is
        // applied for it (see this method's own header comment), but
        // it's worth knowing about when troubleshooting a program that
        // isn't sending what's expected.
        MDBG_LOGF("flush: dropping incomplete command, state=%s", _stateName(_state));
    }
    _reset();
}

// One color of a binary run: to pixel number _runStart + _runIndex (1 =
// first), or -- with start 0, "all" -- collected as a pattern that
// _runFinish() repeats over the whole strip
void CPixelParser::_runPixel(std::uint8_t a, std::uint8_t g, std::uint8_t b, bool is332) {
    if (_runStart == 0) {
        if (is332) {
            std::uint8_t r8, g8, b8;
            rgb332ToRgb888(a, r8, g8, b8);              // same as setPixelRGB332()
            _runPattern.push_back({ r8, g8, b8 });
        } else {
            _runPattern.push_back({ a, g, b });
        }
        return;
    }
    const uint32_t index = static_cast<uint32_t>(_runStart) - 1u + _runIndex;
    if (is332) _strip.setPixelRGB332(index, a);
    else       _strip.setPixelRGB(index, a, g, b);
}

void CPixelParser::_runFinish() {
    if (_runStart != 0 || _runPattern.empty()) return;
    for (uint32_t i = 0; i < _strip.count(); ++i) {
        const auto& c = _runPattern[i % _runPattern.size()];
        _strip.setPixelRGB(i, c[0], c[1], c[2]);
    }
    _runPattern.clear();
}

void CPixelParser::_applyToSelection(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.setPixelRGB(i, r, g, b);
        return;
    }
    if (_selectFrom < 0) return;  // no selection at all -- nothing to do
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {          // pixel numbers start at 1
        if (i >= 1) _strip.setPixelRGB(static_cast<uint32_t>(i - 1), r, g, b);
    }
}

void CPixelParser::_applyToSelection332(std::uint8_t colorByte) {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.setPixelRGB332(i, colorByte);
        return;
    }
    if (_selectFrom < 0) return;
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {          // pixel numbers start at 1
        if (i >= 1) _strip.setPixelRGB332(static_cast<uint32_t>(i - 1), colorByte);
    }
}

void CPixelParser::_onOff() {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.off(i);
        return;
    }
    if (_selectFrom < 0) return;
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {          // pixel numbers start at 1
        if (i >= 1) _strip.off(static_cast<uint32_t>(i - 1));
    }
}

void CPixelParser::_onOn() {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.on(i);
        return;
    }
    if (_selectFrom < 0) return;
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {          // pixel numbers start at 1
        if (i >= 1) _strip.on(static_cast<uint32_t>(i - 1));
    }
}
