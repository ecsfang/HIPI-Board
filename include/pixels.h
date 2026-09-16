#ifndef __PIXELS_H__
#define __PIXELS_H__

#include <cstdint>
#include <vector>
#include <string>
#include "hardware/pio.h"
#include "../PicoLed/PicoLed.hpp"

// ─── Pin/PIO configuration (edit here) ───────────────────────────────────────
// Matches this project's own earlier NeoPixel prototype (pico_main.cpp's
// retired neoTest()) -- PIXEL_PIO uses pio2 (the RP2350's third PIO block,
// not available on the older RP2040) specifically to stay clear of pio0,
// which the HP-IL loop itself already owns (see hpil_pio.hpp) -- no shared
// state machines, no risk of one stealing timing from the other.
#define PIXEL_PIN           26
#define PIXEL_PIO           pio2
#define PIXEL_SM            0

// Not a hard architectural limit -- construct CPixelStrip with a different
// count if more are ever wired up -- just the default this project's own
// global instance (pixelStrip, in pixels.cpp) is built with, and the
// largest pixel index the parser's own 1-byte selectors below can reach
// (start index / run count are each a single byte, 0-255, so this could
// go as high as 256 without changing the protocol at all).
#define PIXEL_COUNT_DEFAULT 32

// ─────────────────────────────────────────────────────────────────────────────
//  CPixelStrip -- thin wrapper around PicoLED's own strip object
//
//  Isolates the actual third-party library calls (PicoLed::addLeds<>(),
//  PicoLed::RGB(), etc.) in this one file -- CPixelParser/CHipiPixel never
//  touch PicoLED's own API directly. If a different WS2812 library is ever
//  substituted, only this class's own implementation (pixels.cpp) needs to
//  change.
// ─────────────────────────────────────────────────────────────────────────────
class CPixelStrip {
public:
    CPixelStrip(uint pin, uint count, PIO pio = PIXEL_PIO, uint sm = PIXEL_SM);

    uint32_t count() const { return count_; }

    // colorByte: RGB332 -- bits [7:5]=R (3 bits), [4:2]=G (3 bits), [1:0]=B
    // (2 bits) -- scaled up to 8 bits per channel; see pixels.cpp's own
    // rgb332ToRgb888(). Silently ignored if index is out of range (matches
    // CLedParser's own "ignore, don't crash" convention elsewhere).
    void setPixelRGB332(uint32_t index, std::uint8_t colorByte);

    // Full 24-bit color, one raw byte per channel.
    void setPixelRGB(uint32_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    // Reads back a pixel's CURRENTLY VISIBLE color (black while off(),
    // even though its own "restore" color is still remembered separately
    // -- see on()'s own comment) -- (0,0,0) if index is out of range.
    // Deliberately backed by this class's own currentR_/G_/B_ cache
    // rather than PicoLED's own getPixelColor(), which isn't const-
    // qualified in the real library and whose Color struct doesn't
    // expose plain .r/.g/.b fields -- avoids depending on either.
    void getPixelColor(uint32_t index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const;

    // Implements the ASCII 'C'/'O' commands' own semantics: off() blacks
    // the pixel out WITHOUT forgetting its last real color; on() restores
    // that remembered color (white 0xFFFFFF if none was ever set).
    void off(uint32_t index);
    void on(uint32_t index);

    // Whole-strip brightness, 0-100% -- scaled to PicoLED's own 0-255
    // range. Does NOT itself call show() -- see that method's own note.
    void setBrightness(std::uint8_t pct);

    // All pixels off (black). Not visible until show() is also called.
    void clear();

    // Pushes the current pixel buffer (and brightness) out to the physical
    // strip -- nothing set via setPixelRGB332()/setPixelRGB()/off()/on()/
    // clear()/setBrightness() above is actually visible until this runs.
    void show();

private:
    void _remember(uint32_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void _setCurrent(uint32_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    // Logs (bExtTrace only) an index that got silently dropped for being
    // out of range -- cheap enough to call unconditionally from every
    // guard below, since the check itself (index >= count_) already has
    // to happen regardless.
    void _traceOutOfRange(uint32_t index, const char* what) const;

    PicoLed::PicoLedController strip_;
    uint32_t count_;  // cached at construction -- see count()'s own comment
    // Per-pixel "last non-off color" cache -- on() restores from here (see
    // its own comment above); PicoLED's own getPixelColor() can't serve
    // this purpose since off() and clear() both actually zero the real
    // pixel buffer.
    std::vector<std::uint8_t> lastR_, lastG_, lastB_;
    // Per-pixel "currently visible" cache -- backs getPixelColor() (see
    // its own comment above). Updated by every call that actually changes
    // what's showing (setPixelRGB(), setPixelRGB332(), off(), on(),
    // clear()) -- unlike lastR_/G_/B_ above, off()/clear() DO update this.
    std::vector<std::uint8_t> currentR_, currentG_, currentB_;
};

// The project's single, global pixel strip instance (mirrors leds.cpp's
// own ledPower/ledStatus/etc. globals) -- defined in pixels.cpp.
extern CPixelStrip pixelStrip;

// ─────────────────────────────────────────────────────────────────────────────
//  CPixelParser — feed one byte at a time
//
//  Two distinct sub-protocols, chosen by the very first byte of a group:
//
//  1) ASCII overview commands (human-typeable, mirrors CLedParser's own
//     style) -- <pixels><cmd>[<params>], groups separated by space/newline/
//     semicolon/null:
//
//       Pixels:  '0' = all, decimal digits = a single index, "N-M" = an
//                inclusive range (e.g. "3-10")
//
//       Commands:
//         C            off (black)
//         O            on (restores each pixel's own last-set color; white
//                      0xFFFFFF if none was ever set)
//         Sn           GLOBAL strip brightness, n% (0-100) -- ignores any
//                      pixel selection, always the whole strip
//         X<byte>      set color, ONE raw byte (RGB332) -- NOT ASCII
//                      digits; the literal byte value right after 'X'
//         Y<r><g><b>   set color, THREE raw bytes (24-bit RGB) -- again
//                      literal byte values, not ASCII digits
//
//       Examples:
//         "0C"             all pixels off
//         "0S50"           strip brightness to 50%
//         "5X<byte>"       pixel 5 to an RGB332 color
//         "3-10X<byte>"    pixels 3..10 to the same RGB332 color
//         "0Y<r><g><b>"    all pixels to the same 24-bit color
//
//  2) Binary "run" mode for detailed, sequential per-pixel colors -- one
//     command byte selects the format, then an explicit count (so the
//     parser always knows exactly how many more bytes to expect -- every
//     byte value 0-255 is a legitimate color, so there's no free value
//     left over to use as an in-band terminator the way group-ending
//     spaces/etc. work for the ASCII commands above):
//
//       '#' <count> <startIndex> <color0> ... <color(count-1)>   -- RGB332,
//           1 byte per pixel
//       '@' <count> <startIndex> <r0><g0><b0> ... <r_n><g_n><b_n> -- 24-bit
//           RGB, 3 bytes per pixel
//
//     Both apply (and show()) once the full run has been consumed. count=0
//     is valid (applies nothing, just consumes startIndex and returns to
//     the idle state) rather than being treated as "no limit".
// ─────────────────────────────────────────────────────────────────────────────
class CPixelParser {
public:
    explicit CPixelParser(CPixelStrip& strip) : _strip(strip) { _reset(); }

    void feed(std::uint8_t c);

    // Call after the last byte of a transmission to flush any pending
    // ASCII group (mirrors CLedParser::flush()). Binary run-mode state
    // deliberately does NOT get force-applied here -- an interrupted run
    // (fewer bytes arrived than the count promised) has no well-defined
    // partial meaning, so it's simply left pending for more bytes rather
    // than guessed at.
    void flush();

private:
    enum class State {
        Idle,             // awaiting pixel-selector digits or a command letter
        AwaitBrightness,  // 'S' seen, awaiting decimal digits
        AwaitX,           // 'X' seen, awaiting exactly 1 raw color byte
        AwaitY1, AwaitY2, AwaitY3,  // 'Y' seen, awaiting 3 raw color bytes
        AwaitRunCount,    // '#' or '@' seen, awaiting the 1-byte count
        AwaitRunStart,    // count read, awaiting the 1-byte start index
        AwaitRun332,      // '#'-run: awaiting the next RGB332 color byte
        AwaitRunR, AwaitRunG, AwaitRunB,  // '@'-run: awaiting r/g/b in turn
    };

    CPixelStrip& _strip;
    State    _state;

    // Pixel selection (ASCII mode)
    bool     _selectAll;
    int32_t  _selectFrom, _selectTo;  // -1 = not set; single index has _selectTo == -1

    // Numeric accumulation (brightness only -- X/Y take raw bytes, not digits)
    int32_t  _paramVal;
    bool     _hasDigit;

    // Binary run state
    bool         _run332;      // true = '#' (RGB332 run), false = '@' (24-bit run)
    std::uint8_t _runCount, _runStart, _runIndex;
    std::uint8_t _runR, _runG;  // partial 24-bit color while awaiting the rest

    void _reset();
    void _finalize();                 // apply the current ASCII group
    void _applyToSelection(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void _applyToSelection332(std::uint8_t colorByte);
    void _onOff();
    void _onOn();

    // Renders the current pixel selection as a short, readable string for
    // trace messages -- "all", "5", or "3-10". Used only when bTrace is on
    // (see feed()'s own trace calls) -- cheap enough not to bother gating
    // the call itself behind bTrace too.
    std::string _selectionDesc() const;

    // Short, human-readable name for a parser state -- used only by
    // feed()'s own byte-level bExtTrace line.
    static const char* _stateName(State s);
};

#endif//__PIXELS_H__
