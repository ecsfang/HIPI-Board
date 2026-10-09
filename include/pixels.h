#ifndef __PIXELS_H__
#define __PIXELS_H__

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <functional>
#include "hardware/pio.h"
#include "../PicoLED/PicoLed.hpp"
#include "../PicoLED/PicoLedEffect.hpp"

// ─── Pin/PIO configuration (edit here) ───────────────────────────────────────
// PIXEL_PIO is pio1, to stay clear of pio0, which the HP-IL loop owns (see
// hpil_pio.hpp) -- no shared state machines, no risk of one stealing timing
// from the other. NOT pio2 (the RP2350's third PIO block): the PicoLED
// library only accepts pio0/pio1 and throws InvalidPioBlock otherwise --
// and since pixelStrip is a global object, that happens before main() and
// the firmware never starts.
#define PIXEL_PIN           26
#define PIXEL_PIO           pio1

// The order the LED strip takes its color bytes in. Classic WS2812B want
// green-red-blue (PicoLed::FORMAT_GRB), but many strips sold as WS2812/
// "NeoPixel" take red-green-blue -- the one on the HIPI board does: with
// FORMAT_GRB, red and green came out swapped (224 = green, 28 = red).
// If red and green are swapped on your strip, switch to the other one.
#ifndef PIXEL_COLOR_ORDER
#define PIXEL_COLOR_ORDER   PicoLed::FORMAT_GRB
#endif
#define PIXEL_SM            0

// Not a hard architectural limit -- construct CPixelStrip with a different
// count if more are ever wired up -- just the default this project's own
// global instance (pixelStrip, in pixels.cpp) is built with, and the
// largest pixel index the parser's own 1-byte selectors below can reach
// (start index / run count are each a single byte, 0-255, so this could
// go as high as 256 without changing the protocol at all).
#define PIXEL_COUNT_DEFAULT 32

// How many of them are really connected is told by the controller ("0N8",
// see CPixelParser) -- HIPI can't detect it -- and kept in CONFIG.TXT
// (pixel_count). Until then: the five on the board (D12-D16). The board's
// pixels and a strip on the connector get the same data, so the board's
// show what the strip's first five show: n is the larger of the two.
#define PIXEL_COUNT_INITIAL 5

// Current limit: the strip has no supply of its own, it runs off the
// board's 5 V (USB). show() lowers the brightness for a frame that would
// draw more than this (e.g. many pixels at full white). A WS2812 draws
// about 20 mA per colour channel at full level.
#define PIXEL_POWER_LIMIT_MA  400
#define PIXEL_MA_PER_CHANNEL  20

// Effects running at the same time (each on its own range of pixels)
#define PIXEL_MAX_EFFECTS     4
// Effects are animated at most this often
#define PIXEL_FRAME_MS        20

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

    // The pixels really connected (1..capacity()): "all" means these, and
    // higher pixel numbers are ignored. The buffer itself always holds
    // capacity() pixels.
    uint32_t count() const { return active_; }
    uint32_t capacity() const { return count_; }
    // Sets count(); pixels above a lower count are switched off (and any
    // effect running there stops). Calls the count-changed callback (to
    // save it) unless notify is false.
    void setActiveCount(uint32_t n, bool notify = true);
    void setCountChangedCallback(std::function<void(uint32_t)> cb) { countChanged_ = std::move(cb); }

    // ── Effects (PicoLED's effect classes, see CPixelParser's 'E') ──────
    // Starts effect `type` (a letter: C M S B F O R) on pixels first..last
    // (0-based, inclusive) with the given parameters: groups (':') of
    // numbers (','); an empty group means "default". Replaces effects it
    // overlaps. Returns false (and logs why) if it can't.
    bool startEffect(uint32_t first, uint32_t last, char type,
                     const std::vector<std::vector<double>>& params);
    // Stops the effects overlapping first..last and switches their pixels
    // off. True if there was one.
    bool stopEffects(uint32_t first, uint32_t last);
    void stopAllEffects() { stopEffects(0, count_ - 1); }
    // Main loop: animates the running effects
    void poll();

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
    // range. Takes effect at the next show() (which may lower it further,
    // see PIXEL_POWER_LIMIT_MA).
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
    uint32_t count_;   // the buffer size (capacity())
    uint32_t active_;  // pixels really connected (count())
    std::function<void(uint32_t)> countChanged_;
    std::uint8_t brightness_ = 255;   // set with setBrightness() (0-255)
    std::uint8_t applied_ = 255;      // last given to PicoLED (after the current limit)

    struct Effect {
        uint32_t first, last;                         // 0-based, inclusive
        std::unique_ptr<PicoLed::PicoLedEffect> fx;
    };
    std::vector<Effect> effects_;
    uint32_t nextFrameMs_ = 0;
    void _blank(uint32_t first, uint32_t last);
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
//       Pixels:  '0' = all, decimal digits = a single pixel NUMBER -- the
//                first pixel is 1 -- and "N-M" an inclusive range (e.g.
//                "3-10").
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
//         Nn           how many pixels are connected (1-32) -- ignores
//                      any selection; saved in CONFIG.TXT. "All" (0) means
//                      these n, higher pixel numbers are ignored.
//         E<t><params> start effect t on the selection (no selection or
//                      0 = all); "E" alone stops the effects there.
//                      Parameters are decimal numbers: ':' between
//                      parameters, ',' between the colours of a palette,
//                      an empty one is the default. Colours are RGB332
//                      numbers (224 red, 28 green, 3 blue, 255 white).
//           EC col:speed:length:fade         comet bouncing back and forth
//           EM pal:length:speed:spacing      marquee (rolling colour bands)
//           ES pal:rate:fade                 stars twinkling
//           EB balls:pal:gravity:fade        bouncing balls
//           EF intensity:spread:cooling      fire (needs 3+ pixels)
//           EO rate                          fade out to black
//           ER speed:step                    rolling rainbow
//         Setting pixels with C/O/X/Y/#/@ stops an effect running on them.
//
//       Examples:
//         "0C"             all pixels off
//         "0S50"           strip brightness to 50%
//         "1X<byte>"       the first pixel to an RGB332 color
//         "3-10X<byte>"    pixels 3..10 to the same RGB332 color
//         "0Y<r><g><b>"    all pixels to the same 24-bit color
//         "0N10"           10 pixels are connected
//         "1-5ES255,252:4" stars on pixels 1..5, white and yellow, 4 a second
//         "6-10EC224:12"   a red comet on pixels 6..10, 12 pixels a second
//         "0E"             stop all effects
//
//  2) Binary "run" mode for detailed, sequential per-pixel colors -- one
//     command byte selects the format, then an explicit count (so the
//     parser always knows exactly how many more bytes to expect -- every
//     byte value 0-255 is a legitimate color, so there's no free value
//     left over to use as an in-band terminator the way group-ending
//     spaces/etc. work for the ASCII commands above):
//
//       '#' <count> <start> <color0> ... <color(count-1)>   -- RGB332,
//           1 byte per pixel
//       '@' <count> <start> <r0><g0><b0> ... <r_n><g_n><b_n> -- 24-bit
//           RGB, 3 bytes per pixel
//
//     <start> is a pixel number like in the ASCII commands: 1 = the first
//     pixel. 0 = all: the <count> colors are repeated over the whole strip
//     (count=1: the whole strip one color; count=3: a 3-color pattern).
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
        AwaitCount,       // 'N' seen, awaiting decimal digits
        AwaitEffectType,  // 'E' seen, awaiting the effect letter (or the end: stop)
        AwaitEffectParams,// effect letter seen, collecting its parameters
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
    std::vector<std::array<std::uint8_t, 3>> _runPattern;   // start 0: colors to repeat
    void _runPixel(std::uint8_t a, std::uint8_t g, std::uint8_t b, bool is332);
    void _runFinish();

    // Effect being given ('E')
    char        _effectType;
    std::string _effectParams;
    void _startEffect();              // the collected 'E' command
    void _stopEffectsInSelection();
    // The current selection as 0-based first..last within count() --
    // false if there is none (or it's beyond count())
    bool _selectionRange(uint32_t& first, uint32_t& last) const;
    // Before a command sets pixels: stops the effects running there
    void _claimSelection();

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
