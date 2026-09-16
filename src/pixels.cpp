#include "pixels.h"
#include "usb_serial.h"
#include <algorithm>
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

// ─── CPixelStrip ────────────────────────────────────────────────────────────

CPixelStrip::CPixelStrip(uint pin, uint count, PIO pio, uint sm)
    : strip_(PicoLed::addLeds<PicoLed::WS2812B>(pio, sm, pin, count, PicoLed::FORMAT_GRB)),
      count_(count),
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
    if (bExtTrace) {
        LOGF("\r\n[PIXEL] %s: index %lu out of range (strip has %lu pixel(s)) -- ignored",
             what, static_cast<unsigned long>(index), static_cast<unsigned long>(count_));
    }
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
    strip_.setBrightness(static_cast<std::uint8_t>((static_cast<uint16_t>(clamped) * 255) / 100));
}

void CPixelStrip::clear() {
    strip_.clear();
    // NOT clearing lastR_/G_/B_ here either -- "0C" then "0O" should bring
    // everything back exactly as it was, same reasoning as off() above.
    for (uint32_t i = 0; i < count_; ++i) _setCurrent(i, 0, 0, 0);
}

void CPixelStrip::show() {
    strip_.show();
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
            LOGF("\r\n[PIXEL] feed: 0x%02X ('%c') in state %s", c, c, _stateName(_state));
        else
            LOGF("\r\n[PIXEL] feed: 0x%02X in state %s", c, _stateName(_state));
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
            _runStart = c;
            _runIndex = 0;
            if (_runCount == 0) {
                if (bTrace) LOGF("\r\n[PIXEL] run(%s): count=0, start=%u -- nothing to do",
                                 _run332 ? "RGB332" : "RGB24", _runStart);
                _reset();
                return;
            }  // count=0: nothing to apply
            if (bTrace) LOGF("\r\n[PIXEL] run(%s): count=%u start=%u",
                             _run332 ? "RGB332" : "RGB24", _runCount, _runStart);
            _state = _run332 ? State::AwaitRun332 : State::AwaitRunR;
            return;

        case State::AwaitRun332:
            _strip.setPixelRGB332(static_cast<uint32_t>(_runStart) + _runIndex, c);
            if (++_runIndex >= _runCount) {
                _strip.show();
                if (bTrace) LOGF("\r\n[PIXEL] run(RGB332) complete: %u pixel(s) from %u",
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
            _strip.setPixelRGB(static_cast<uint32_t>(_runStart) + _runIndex, _runR, _runG, c);
            if (++_runIndex >= _runCount) {
                _strip.show();
                if (bTrace) LOGF("\r\n[PIXEL] run(RGB24) complete: %u pixel(s) from %u",
                                 _runCount, _runStart);
                _reset();
            } else {
                _state = State::AwaitRunR;
            }
            return;

        // ── 'X'/'Y' ASCII commands' own raw color byte(s) -- also literal,
        // never interpreted as digits/separators. ──────────────────────
        case State::AwaitX:
            if (bTrace) LOGF("\r\n[PIXEL] set %s = RGB332 0x%02X", _selectionDesc().c_str(), c);
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
                LOGF("\r\n[PIXEL] set %s = RGB(%u,%u,%u)",
                     _selectionDesc().c_str(), _runR, _runG, c);
            }
            _applyToSelection(_runR, _runG, c);
            _strip.show();
            _reset();
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
                if (bTrace) LOGF("\r\n[PIXEL] brightness -> %ld%%", static_cast<long>(pct));
                _strip.setBrightness(static_cast<std::uint8_t>(pct));
            }
            _strip.show();
            _reset();
            feed(c);
            return;

        // ── Idle: pixel selection digits, '-' range separator, or a
        // command letter. ────────────────────────────────────────────────
        case State::Idle:
            if (c == ' ' || c == '\n' || c == ';' || c == '\0') {
                _reset();  // no active command -- nothing to finalize
                return;
            }
            if (c == '0' && _selectFrom < 0) {
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
                    if (bTrace) LOGF("\r\n[PIXEL] off: %s", _selectionDesc().c_str());
                    _onOff(); _strip.show(); _reset(); return;
                case 'O':
                    if (bTrace) LOGF("\r\n[PIXEL] on: %s", _selectionDesc().c_str());
                    _onOn();  _strip.show(); _reset(); return;
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
                            LOGF("\r\n[PIXEL] ignored stray byte 0x%02X ('%c') in Idle state", c, c);
                        else
                            LOGF("\r\n[PIXEL] ignored stray byte 0x%02X in Idle state", c);
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
        if (bTrace) LOGF("\r\n[PIXEL] brightness -> %ld%% (at flush)", static_cast<long>(pct));
        _strip.setBrightness(static_cast<std::uint8_t>(pct));
        _strip.show();
    } else if (bExtTrace) {
        // Any other pending state at flush() means the transmission ended
        // mid-command -- e.g. '#'/'@' cut off before its run finished, or
        // 'X'/'Y' cut off before its color byte(s) arrived. Nothing is
        // applied for it (see this method's own header comment), but
        // it's worth knowing about when troubleshooting a program that
        // isn't sending what's expected.
        LOGF("\r\n[PIXEL] flush: dropping incomplete command, state=%s", _stateName(_state));
    }
    _reset();
}

void CPixelParser::_applyToSelection(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.setPixelRGB(i, r, g, b);
        return;
    }
    if (_selectFrom < 0) return;  // no selection at all -- nothing to do
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {
        if (i >= 0) _strip.setPixelRGB(static_cast<uint32_t>(i), r, g, b);
    }
}

void CPixelParser::_applyToSelection332(std::uint8_t colorByte) {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.setPixelRGB332(i, colorByte);
        return;
    }
    if (_selectFrom < 0) return;
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {
        if (i >= 0) _strip.setPixelRGB332(static_cast<uint32_t>(i), colorByte);
    }
}

void CPixelParser::_onOff() {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.off(i);
        return;
    }
    if (_selectFrom < 0) return;
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {
        if (i >= 0) _strip.off(static_cast<uint32_t>(i));
    }
}

void CPixelParser::_onOn() {
    if (_selectAll) {
        for (uint32_t i = 0; i < _strip.count(); ++i) _strip.on(i);
        return;
    }
    if (_selectFrom < 0) return;
    const int32_t to = (_selectTo >= 0) ? _selectTo : _selectFrom;
    for (int32_t i = _selectFrom; i <= to; ++i) {
        if (i >= 0) _strip.on(static_cast<uint32_t>(i));
    }
}
