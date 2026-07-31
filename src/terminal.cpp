#include "terminal.h"
#include "usb_serial.h"
#include "tusb.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

extern bool bTrace;

namespace {
// Same 50ms window used throughout this project's other debounce/hide
// timers -- comfortably longer than a terminal program's own multi-byte
// burst for one keypress, short enough that a genuinely bare ESC key
// doesn't feel laggy before it shows up.
constexpr std::uint32_t kEscTimeoutMs = 50;

// "ESC [ <letter>" -- arrow keys. A straight A/B/C/D passthrough --
// pilkeymap.py's own Key_Down/Key_Left entries suggested a B/D swap
// here, but a real hardware test showed Down and Left ending up
// swapped with that in place, so this project's HP-71 evidently uses
// the same A/B/C/D order ANSI does.
char escBracketLetterToHp71(char c) {
    switch (c) {
        case 'A': return 'A';  // Up
        case 'B': return 'B';  // Down
        case 'C': return 'C';  // Right
        case 'D': return 'D';  // Left
        default:  return 0;
    }
}

// "ESC [ <digits> ~" -- edit/function keys. Codes matching what was
// actually decoded from this specific terminal (see the conversation);
// PgUp(5)/PgDn(6)/F7(18)/F8(19)/F9(20) have no HP-71 equivalent in
// pilkeymap.py's keymap_hp71, so they're intentionally left unmapped
// (dropped silently rather than forwarding a meaningless raw sequence).
char escBracketDigitsToHp71(int code) {
    switch (code) {
        case 1:  return 'E';   // Home  -> far left
        case 2:  return 'H';   // Insert -> I/R
        case 3:  return 'G';   // Delete -> -CHAR
        case 16: return 'I';   // F5 -> -LINE
        case 17: return 'O';   // F6 -> LC
        default: return 0;
    }
}

// "ESC O <letter>" -- F1-F4 and End on this terminal.
char escOLetterToHp71(char c) {
    switch (c) {
        case 'P': return 'L';  // F1 -> ATTN
        case 'Q': return 'M';  // F2 -> RUN
        case 'R': return 'N';  // F3 -> CMDS
        case 'S': return 'P';  // F4 -> SST
        case 'F': return 'F';  // End -> far right
        default:  return 0;
    }
}
}  // namespace

void CTerminal::pushKey(std::uint8_t b) {
    if (keyIn_.size() >= kMaxQueuedKeys) return;
    keyIn_.push(b);
    if (bTrace) LOGF("\r\n[TERMINAL] key queued: 0x%02X (queue=%zu)", b, keyIn_.size());
}

void CTerminal::feedRawByte(std::uint8_t b) {
    switch (escState_) {
    case EscState::Idle:
        if (b == 0x1b) {
            escState_ = EscState::SawEsc;
            escDeadline_ = make_timeout_time_ms(kEscTimeoutMs);
        } else {
            // Enter (0x0d) used to get translated to ESC R here, based on
            // pilkeymap.py's Key_Return entry ("# Endline"). A real
            // capture with pyILPER's own reference keyboard proved that
            // wrong: it sends BOTH ESC R and, in a separate poll cycle
            // right after, plain 0x0d for the very same Enter keypress --
            // and neither made "LIST" execute. Only the numeric keypad's
            // Enter (which isn't in pilkeymap.py's dict at all, so it
            // must fall back to sending the key's raw text instead of
            // going through the ESC-sequence keymap) actually worked.
            // So plain 0x0d, forwarded completely unchanged like every
            // other ordinary byte, is what KEYBOARD IS actually wants.
            pushKey(b);
        }
        break;

    case EscState::SawEsc:
        if (b == '[') {
            escState_ = EscState::SawEscBracket;
            escDeadline_ = make_timeout_time_ms(kEscTimeoutMs);
        } else if (b == 'O') {
            escState_ = EscState::SawEscO;
            escDeadline_ = make_timeout_time_ms(kEscTimeoutMs);
        } else {
            // A bare ESC keypress (or ESC followed by something we don't
            // recognize as a sequence start) -- the 71B doesn't need a
            // literal ESC keystroke, so just drop it and reprocess this
            // byte fresh from Idle (it's neither '[' nor 'O' at this
            // point, so this recursion is always exactly one level deep).
            escState_ = EscState::Idle;
            feedRawByte(b);
        }
        break;

    case EscState::SawEscBracket: {
        const char mapped = escBracketLetterToHp71(static_cast<char>(b));
        if (mapped) {
            pushKey(0x1b);
            pushKey(static_cast<std::uint8_t>(mapped));
            escState_ = EscState::Idle;
        } else if (std::isdigit(static_cast<unsigned char>(b))) {
            escDigits_[0] = static_cast<char>(b);
            escDigitLen_ = 1;
            escState_ = EscState::SawEscBracketDigits;
            escDeadline_ = make_timeout_time_ms(kEscTimeoutMs);
        } else {
            // Unrecognized fragment -- not meaningful input for the 71B,
            // drop it rather than forwarding raw escape-sequence bytes.
            escState_ = EscState::Idle;
        }
        break;
    }

    case EscState::SawEscBracketDigits:
        if (std::isdigit(static_cast<unsigned char>(b)) && escDigitLen_ < sizeof(escDigits_) - 1) {
            escDigits_[escDigitLen_++] = static_cast<char>(b);
            escDeadline_ = make_timeout_time_ms(kEscTimeoutMs);
        } else if (b == '~') {
            escDigits_[escDigitLen_] = '\0';
            const int code = std::atoi(escDigits_);
            const char mapped = escBracketDigitsToHp71(code);
            if (mapped) {
                pushKey(0x1b);
                pushKey(static_cast<std::uint8_t>(mapped));
            } else if (bTrace) {
                LOGF("\r\n[TERMINAL] no HP-71 mapping for ESC[%d~ -- dropped", code);
            }
            escState_ = EscState::Idle;
            escDigitLen_ = 0;
        } else {
            escState_ = EscState::Idle;
            escDigitLen_ = 0;
        }
        break;

    case EscState::SawEscO: {
        const char mapped = escOLetterToHp71(static_cast<char>(b));
        if (mapped) {
            pushKey(0x1b);
            pushKey(static_cast<std::uint8_t>(mapped));
        }
        escState_ = EscState::Idle;
        break;
    }
    }
}

IL_CMD_t CTerminal::hpil(IL_CMD_t cmd) {
    // SAI/SDI (device identification) queries are answered generically by
    // CDevice::base() -- not by our own doTalker() -- so justDeliveredData_
    // (see below) doesn't cover them. They're our own manufactured
    // response too, and must be excluded from SRQ the same way: a real
    // capture showed our SAI response (0x4E) getting corrupted into 0x14E
    // on every single routine device-identification poll while a key was
    // queued -- which happens constantly, as part of completely normal
    // bus housekeeping that has nothing to do with keyboard input at all.
    const bool isOwnIdentificationResponse = isTalker() && (cmd == SAI || cmd == SDI);

    IL_CMD_t rtn = CDevice::hpil(cmd);
    // Piggyback our Service Request onto whatever DOE- or IDY-class frame
    // is passing through right now, regardless of whether this frame was
    // actually addressed to us. SRQ is a LIVE condition -- simply "do we
    // have a keystroke waiting" -- not a separate flag that needs manual
    // re-arming (see terminal.h's class comment for why that matters).
    //
    // Deliberately excluded: the ONE frame where doTalker() (called just
    // above, via CDevice::hpil()) just handed back one of our own queued
    // bytes -- justDeliveredData_ marks exactly that call, and only that
    // one. A real capture showed the 71B stop polling entirely partway
    // through delivering ESC R (Enter, a 2-byte translated keystroke)
    // when SRQ appeared on the first byte while the second was still
    // queued behind it -- pyILPER's cls_pilterminal never does this
    // either (its __outdata__ returns the plain data byte, managing SRQ
    // purely through a separate status byte read via SST). Also excluded,
    // for the same reason: our own SAI/SDI response (see above).
    //
    // Gating on isTalker() alone was tried first and seemed to make things
    // worse -- though in hindsight that symptom may well have been THIS
    // same SAI-corruption bug wearing a different hat, not something
    // isTalker() itself explains. Either way, justDeliveredData_ plus the
    // explicit SAI/SDI check is more precise than blanket-gating on
    // isTalker(): it excludes exactly the frames that are our own
    // manufactured responses, and nothing else -- so SRQ still freely
    // piggybacks onto everything else, including other frames seen while
    // isTalker() happens to still be true for an unrelated reason.
    if (!justDeliveredData_ && !isOwnIdentificationResponse &&
        !keyIn_.empty() && (IS_DATA(rtn) || IS_IDLE(rtn))) {
        rtn = static_cast<IL_CMD_t>(rtn | SRQ_BIT);
        if (bTrace) LOGF("\r\n[TERMINAL] hpil: added SRQ_BIT -> 0x%03X", rtn);
    }
    return rtn;
}

void CTerminal::clear(void) {
    if (bTrace) LOGF("\r\n[TERMINAL] clear (DCL/SDC) -- dropping %zu queued key(s)", keyIn_.size());
    while (!keyIn_.empty()) keyIn_.pop();
    justDeliveredData_ = false;
    escState_ = EscState::Idle;
    escDigitLen_ = 0;
    outFlag_ = false;
    outN_ = -1;
}

void CTerminal::doListener(IL_CMD_t cmd, IL_CMD_t *rtn) {
    *rtn = cmd;
    if (IS_DATA(cmd)) {
        translateAndForward(static_cast<std::uint8_t>(cmd & 0xFF));
    }
}

void CTerminal::sendVt(const char* s) {
    while (*s) {
        tud_cdc_n_write_char(ITF_TERMINAL, *s++);
    }
    tud_cdc_n_write_flush(ITF_TERMINAL);
}

// Mirrors Screen::pr_char()'s state machine exactly (same flag_/n_
// structure, same recognized codes) -- see terminal.h's comment on
// outFlag_/outN_ for why a translation is needed at all, not just a
// straight byte relay.
void CTerminal::translateAndForward(std::uint8_t c) {
    if (outFlag_) {
        outFlag_ = false;
        if (outN_ == 0) {
            outPos_[0] = c;
            outN_ = 1;
            outFlag_ = true;
        } else if (outN_ == 1) {
            outPos_[1] = c;
            // Screen::cursor_pos(row=outPos_[1], col=outPos_[0]) -- VT100
            // is 1-indexed, HP-71's own row/col (per Screen.hpp) are 0-
            // indexed, and its cursor-address space is a fixed 32x16
            // grid regardless of the actual column/row count in effect.
            char buf[24];
            std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                          (outPos_[1] % 16) + 1, (outPos_[0] % 32) + 1);
            sendVt(buf);
            outN_ = -1;
        } else if (c == 37) {              // ESC %
            outN_ = 0;
            outFlag_ = true;
        } else if (c == 83) {              // ESC S -> roll up
            sendVt("\n");                  // natural scroll approximation
        } else if (c == 84) {              // ESC T -> roll down
            sendVt("\x1bM");               // VT100 Reverse Index
        } else if (c == 65) {              // ESC A -> up
            sendVt("\x1b[A");
        } else if (c == 66) {              // ESC B -> down
            sendVt("\x1b[B");
        } else if (c == 67) {              // ESC C -> right
            sendVt("\x1b[C");
        } else if (c == 68) {              // ESC D -> left (see the class
            sendVt("\x1b[D");              // comment -- the whole reason
                                            // this translator exists)
        } else if (c == 72) {              // ESC H -> home
            sendVt("\x1b[H");
        } else if (c == 60) {              // ESC < -> cursor off
            sendVt("\x1b[?25l");
        } else if (c == 62) {              // ESC > -> cursor on
            sendVt("\x1b[?25h");
        } else if (c == 81 || c == 78) {   // ESC Q / ESC N -> insert mode on
            sendVt("\x1b[4h");
        } else if (c == 82) {              // ESC R -> replace (insert off)
            sendVt("\x1b[4l");
        } else if (c == 69) {              // ESC E -> clear screen + home
            sendVt("\x1b[2J\x1b[H");
        } else if (c == 74) {              // ESC J -> clear to end of screen
            sendVt("\x1b[J");
        } else if (c == 75) {              // ESC K -> clear to end of line
            sendVt("\x1b[K");
        } else if (c == 76) {              // ESC L -> insert line
            sendVt("\x1b[L");
        } else if (c == 77) {              // ESC M -> delete line
            sendVt("\x1b[M");
        } else if (c == 79) {              // ESC O -> delete character
            sendVt("\x1b[P");
        }
        // ESC [ / ESC ] (91/93, our own font-size 0/1) have no meaningful
        // VT100 equivalent -- a terminal program's font isn't something
        // to remote-control -- so those, and anything else unrecognized,
        // are just silently swallowed rather than leaking a stray '['/']'
        // onto the screen.
        return;
    }

    if (c == 27) {                         // ESC -- always starts a
        outFlag_ = true;                   // sequence for us, same as
        outN_ = -1;                        // Screen::pr_char()
        return;
    }
    // Everything else -- printable characters, and BS/LF/CR (8/10/13) --
    // means the same thing to a real VT100 terminal as it does to the
    // 71B, so it's forwarded completely unchanged.
    tud_cdc_n_write_char(ITF_TERMINAL, static_cast<char>(c));
    tud_cdc_n_write_flush(ITF_TERMINAL);
}

void CTerminal::doTalker(IL_CMD_t cmd, IL_CMD_t *rtn) {
    justDeliveredData_ = false;   // reset every call; set true below only
                                  // for the one case that actually delivers
                                  // one of our own queued bytes
    if (bTrace && !IS_ROUTINE(cmd)) {
        LOGF("\r\n[TERMINAL] doTalker called: cmd=0x%03X (SDA=%d SST=%d NRD=%d) keyQ=%zu",
             cmd, cmd == SDA, cmd == SST, cmd == NRD, keyIn_.size());
    }

    if (cmd == NRD) return;   // just an acknowledgment, no state to update

    if (cmd == SST) {
        // Status poll -- bit 6 (SRQ/rsv) and bit 0 (our own "data
        // available" convenience flag) are both just !keyIn_.empty(),
        // computed fresh every time. No manual clearing needed: the very
        // next check (via hpil() above, or another SST) automatically
        // reflects whatever the queue's real state is by then.
        const std::uint8_t status = keyIn_.empty() ? 0 : 0x41;
        *rtn = status;
        if (bTrace) LOGF("\r\n[TERMINAL] SST -> status=0x%02X (keyQ=%zu)", status, keyIn_.size());
        return;
    }

    if (cmd == SDA) {
        // Deliver exactly one byte per SDA -- a real capture against
        // pyILPER's own reference implementation proved that each
        // keystroke byte is its own one-shot transaction: SDA -> byte,
        // NRD -> ack, the echoed byte looping back -> ETO, and the NEXT
        // byte (even a second byte of the SAME translated multi-byte
        // keystroke, like ESC+A for an arrow key) always needs a FRESH
        // SDA, never delivered via the echo. This project's own
        // CPlotter uses a genuinely different, continuous multi-byte
        // model for its OP/OA query responses (one long string read in
        // one sitting) -- that pattern doesn't apply here, and using it
        // was exactly what broke multi-byte translated keystrokes (the
        // first byte would deliver fine, but the second -- delivered via
        // the echo instead of a fresh SDA -- confused the 71B enough
        // that it stopped polling for anything further at all).
        if (keyIn_.empty()) {
            *rtn = ETO;
        } else {
            *rtn = static_cast<IL_CMD_t>(keyIn_.front());
            keyIn_.pop();
            justDeliveredData_ = true;
            if (bTrace) {
                LOGF("\r\n[TERMINAL] doTalker: key 0x%02X -> HP-71B (queue left=%zu)",
                     static_cast<unsigned>(*rtn), keyIn_.size());
            }
        }
        return;
    }

    if (IS_DATA(cmd)) {
        // The echoed copy of the byte we just sent, looping back around
        // the physical bus -- always answered with ETO, never with
        // another queued byte (see the SDA case's comment above).
        *rtn = ETO;
    }
}

void CTerminal::preProc(IL_CMD_t cmd) {
    if (bTrace && !IS_ROUTINE(cmd)) {
        LOGF("\r\n[TERMINAL] preProc: cmd=0x%03X isListener=%d isTalker=%d addr=%d keyQ=%zu",
             cmd, isListener(), isTalker(), addr(), keyIn_.size());
    }
}

void CTerminal::idle(void) {
    while (keyIn_.size() < kMaxQueuedKeys && tud_cdc_n_available(ITF_TERMINAL) > 0) {
        const int c = tud_cdc_n_read_char(ITF_TERMINAL);
        if (c < 0) break;   // shouldn't happen given the available() check, but be safe
        feedRawByte(static_cast<std::uint8_t>(c));
    }
    // A bare ESC keypress (or a sequence that got cut off) looks
    // identical to "still waiting for more bytes" until either more
    // arrive or enough time has passed that we give up. Neither needs
    // forwarding to the 71B (see terminal.h), so this just drops
    // whatever was buffered and resets.
    if (escState_ != EscState::Idle && time_reached(escDeadline_)) {
        if (bTrace) LOGF("\r\n[TERMINAL] esc sequence timed out -- dropped");
        escState_ = EscState::Idle;
        escDigitLen_ = 0;
    }
}

void CTerminal::show(void) {
    CDevice::show();
    LOGF("\r\n\tkeyIn queue:%zu cdc_connected:%d",
         keyIn_.size(), tud_cdc_n_connected(ITF_TERMINAL));
}