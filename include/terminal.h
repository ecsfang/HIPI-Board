#ifndef __TERMINAL_H__
#define __TERMINAL_H__

// CTerminal -- bridges an HP-71B's "KEYBOARD IS :n"/"DISPLAY IS :n" to a
// USB CDC serial port (a third USB serial port, ITF_TERMINAL -- see
// hpil.h -- separate from the debug console and PILBox's own forwarding
// port), so a terminal program on the PC (e.g. minicom) becomes the
// 71B's keyboard and display.
//
// Design confirmed against pyILPER's cls_pilterminal (pilterminal.py) --
// a real, working HP-IL terminal device implementation -- after our own
// SRQ attempt (based purely on the formal protocol spec) got stuck after
// exactly one keystroke:
//   DISPLAY IS :n  -- the 71B is talker, we're LISTENER: every character
//                     it sends gets written straight out to the USB CDC
//                     port (doListener()).
//   KEYBOARD IS :n -- the 71B is listener, we're TALKER: it addresses us
//                     and polls (SDA) for the next keystroke; we answer
//                     with whatever's next in keyIn_, or ETO if nothing's
//                     been typed yet (doTalker()).
//
// Two things pyILPER's reference does differently from our first attempt
// (this took several rounds of real captures to get right):
//   1. Service Request is a LIVE, computed condition -- "SRQ asserted"
//      simply means "keyIn_ is non-empty right now", re-evaluated on
//      every check. There's no separate one-shot "rsv" flag that a
//      status query has to explicitly clear/re-arm (our first attempt's
//      rsv_ could get stuck true forever if the controller drained the
//      queue via SDA without ever sending SST, since only SST cleared
//      it -- exactly what a real capture showed happening).
//   2. Every keystroke BYTE needs its own fresh SDA -- there is no
//      continuous multi-byte drain the way CPlotter's OP/OA responses
//      work (one long string, read across several polls via the byte we
//      just sent echoing back around the physical loop). A real capture
//      against pyILPER's own __outdata__ proved this: SDA delivers one
//      byte, NRD acknowledges, and the echoed copy of that byte looping
//      back always gets ETO -- never the next queued byte. Even a
//      2-byte translated keystroke (ESC+A for an arrow key) needs TWO
//      separate SDA cycles, one per byte. Our first attempt copied
//      CPlotter's continuous-drain pattern here too, and it's exactly
//      what broke every multi-byte keystroke: the first byte delivered
//      fine, but the second (sent via the echo instead of a fresh SDA)
//      confused the 71B enough to stop polling for anything further.
//
// Per the HP-IL protocol spec's SR function (confirmed against
// hp82166-is-en.pdf): a device requests service by setting the SRQ bit
// (SRQ_BIT, see hpil.h) on ANY DOE- or IDY-class frame it sources or
// retransmits. This is genuinely just an ordinary data/idle frame with
// one extra bit set -- it's what we'd been seeing (and, before
// understanding this, trace-filtering as routine noise) as "DSR"/"ESR"
// traffic whenever an HP-71B was on the loop. CMD/RDY-class frames
// (LAD/TAD/UNL/UNT/AAU/SDA/SST/...) have no SRQ bit position and must
// never have it set -- see hpil() below, the one place that applies it.

#include "hpil.h"
#include "pico/time.h"
#include <cstdint>
#include <queue>

class CTerminal : public CDevice {
public:
    // AID 0x4E ("general interface", class 4x) per the formal Accessory
    // ID table. pyILPER's cls_pilterminal actually uses 0x3E ("general
    // display", class 3x) -- we matched that briefly, but it collided
    // with CDisplay's own 0x3E, making AID-based device selection from
    // the 71B pick whichever device it happened to find first. 0x4E
    // keeps every device's AID distinct.
    CTerminal(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau = 31)
        : CDevice(name, _sai, _aau, TERMINAL) {}

    // Wraps CDevice::hpil() to piggyback our Service Request bit onto the
    // returned frame -- overriding this (rather than just doListener()/
    // doTalker()) is what lets it apply to any passing DOE/IDY frame, not
    // just ones we're specifically addressed for.
    IL_CMD_t hpil(IL_CMD_t cmd);

    void clear(void);                          // DCL/SDC -- drop queued keystrokes
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);  // DISPLAY IS: HP-71B -> us -> USB CDC
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn);    // KEYBOARD IS: one fresh SDA per byte, + SST
    void idle(void);                           // drains USB CDC RX, translates escape sequences, sets keyIn_

    // Debug: logs every non-routine frame (see hipi.cpp's IS_ROUTINE)
    // together with our current addressing state, regardless of whether
    // this device is actually addressed by it. Temporary-ish diagnostic
    // aid; safe to remove once KEYBOARD IS is confirmed solid.
    void preProc(IL_CMD_t cmd);

    void show(void);                           // extended-trace detail

private:
    // Keystrokes read from the USB CDC port (minicom etc.), waiting to be
    // polled by KEYBOARD IS. Capped so a 71B that never actually reads
    // can't make this grow forever -- same defensive reasoning as the
    // plotter's LB label-length cap.
    static constexpr std::size_t kMaxQueuedKeys = 64;
    std::queue<std::uint8_t> keyIn_;

    // Set true by doTalker() for exactly the one hpil() call where it
    // just handed back one of our own queued bytes (in response to an
    // explicit SDA -- see doTalker()'s comment on why each keystroke
    // byte needs its own fresh SDA, unlike CPlotter's continuous
    // multi-byte OP/OA responses) -- not for the echoed copy of that
    // byte looping back afterward, which just gets ETO. See hpil()'s
    // comment for why SRQ must never piggyback onto that specific frame,
    // but should still freely piggyback onto everything else, including
    // other frames seen while isTalker() happens to still be true (a
    // real capture showed the controller can stay "talker" toward us for
    // a while after an ETO before formally ending the session -- gating
    // purely on isTalker() suppressed SRQ for that whole window instead
    // of just the one frame that actually needed it suppressed).
    bool justDeliveredData_ = false;

    // ── Special-key escape sequence translation ──────────────────────
    // The terminal program (minicom etc.) sends its own ANSI/xterm-style
    // multi-byte escape sequences for arrow/function/edit keys (e.g. Up
    // = ESC [ A), but the 71B expects its own bespoke 2-byte convention
    // (ESC + a single letter -- e.g. Up = ESC A), matching what
    // Screen::cursor() already parses on the *output* side and what
    // pyILPER's pilkeymap.py encodes for its own (Qt-keyevent-based, not
    // directly reusable here) keyboard handling. This buffers bytes
    // starting with ESC (0x1b) until a known sequence completes (then
    // translates and queues it) or a short timeout/mismatch occurs (then
    // the whole fragment -- including a bare ESC keypress on its own --
    // is simply dropped, not forwarded: the 71B doesn't need a literal
    // ESC keystroke, and a broken/unrecognized fragment isn't meaningful
    // input for it either).
    enum class EscState { Idle, SawEsc, SawEscBracket, SawEscBracketDigits, SawEscO };
    EscState escState_ = EscState::Idle;
    char escDigits_[4] = {0, 0, 0, 0};
    std::uint8_t escDigitLen_ = 0;
    absolute_time_t escDeadline_;   // matches boardui.cpp's own hide-timer idiom

    void feedRawByte(std::uint8_t b);
    void pushKey(std::uint8_t b);

    // ── DISPLAY IS: HP-71 escape convention -> VT100/ANSI ─────────────
    // The 71B's DISPLAY IS output uses the same bespoke 2-byte escape
    // convention Screen::cursor() parses (ESC + a letter -- e.g. cursor
    // left = ESC D), and leans on it heavily for CURSOR MOVEMENT rather
    // than sending literal newlines/spaces (e.g. walking a list of
    // strings by moving the cursor back to the start column with a run
    // of ESC D's, then overwriting). A generic ANSI terminal like minicom
    // doesn't understand that convention -- worse, several of these
    // single-byte codes collide with *real* VT100 C1 control codes: ESC D
    // is VT100's IND (Index -- move down a line, scrolling if needed), so
    // every "cursor left" the 71B sends gets misread as "new line",
    // producing exactly the run of blank lines seen in a real capture.
    // Screen::pr_char()'s state machine is mirrored here (same flag_/n_
    // structure) to parse the identical byte stream, but emits translated
    // VT100/ANSI sequences to USB CDC instead of drawing to the panel.
    bool outFlag_ = false;   // mirrors Screen::flag_
    int outN_ = -1;          // mirrors Screen::n_ (cursor-position byte count)
    std::uint8_t outPos_[2] = {0, 0};

    void translateAndForward(std::uint8_t c);
    void sendVt(const char* s);
};

#endif//__TERMINAL_H__