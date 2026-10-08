#define MODULE "SIGNALS"
#include "hpil_diag.h"
#include "hpil.h"
#include "hpil_pio.hpp"          // IN_P_PIN / IN_M_PIN: the receiver's "plus" and "minus"
#include "hpil_diag.pio.h"
#include "plotterview.h"
#include "usb_serial.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hipi {

namespace {

// Pins as named in the schematic: pIn (U3A, positive pulses) on GPIO13,
// mIn (U3B, negative pulses) on GPIO12. The receiver (hpil.pio) uses
// IN_P_PIN as its "plus" -- see the view's labels.
constexpr uint kPinPIn = 13;
constexpr uint kPinMIn = 12;
constexpr uint kScopeBase = 12;               // GPIO12 = bit 0, GPIO13 = bit 1 of each sample

PIO const kPio = pio2;
uint smScope_ = 0;
uint scopeOffset_ = 0;
int dma_ = -1;

// ── Scope ───────────────────────────────────────────────────────────────
constexpr std::uint32_t kSampleHz = 8000000;          // 0.125 µs per sample
constexpr int kWords = 2048;                          // 16 samples each: 32768 samples, ~4.1 ms
std::uint32_t buf_[kWords];
bool armed_ = false, haveCapture_ = false;
absolute_time_t nextArm_ = nil_time;
std::uint32_t captureNo_ = 0;

inline bool sampleBit(int i, int bit) {               // bit 0 = GPIO12, 1 = GPIO13
    return (buf_[i / 16] >> ((i % 16) * 2 + bit)) & 1u;
}
inline bool pinAt(int i, uint gpio) { return sampleBit(i, gpio == kScopeBase ? 0 : 1); }
constexpr int kSamples = kWords * 16;

void arm() {
    pio_sm_set_enabled(kPio, smScope_, false);
    pio_sm_clear_fifos(kPio, smScope_);
    pio_sm_restart(kPio, smScope_);
    pio_sm_exec(kPio, smScope_, pio_encode_jmp(scopeOffset_ + hpil_scope_offset_trig));
    dma_channel_config c = dma_channel_get_default_config(static_cast<uint>(dma_));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(kPio, smScope_, false));
    dma_channel_configure(static_cast<uint>(dma_), &c, buf_, &kPio->rxf[smScope_], kWords, true);
    pio_sm_set_enabled(kPio, smScope_, true);
    armed_ = true;
}

// ── Decoding the capture like the receiver (hpil.pio) does ──────────────
// plus = the receiver's IN_P_PIN, minus = IN_M_PIN. A frame starts with a
// plus pulse; then 11 bits, each found by its plus pulse (waited for up to
// ~16 µs) and read from minus 0.5 µs (2 PIO cycles at 4 MHz) after that
// pulse ends: minus right after plus = 1, minus before plus = 0. The first
// bit carries the sync (an extra pulse pair in front).
constexpr int kSampleAfterPlus = 4;                   // 0.5 µs
constexpr int kBitTimeout = 128;                      // ~16 µs

inline int level(int i) {                             // +1 plus, -1 minus, 0 none
    if (i < 0 || i >= kSamples) return 0;
    if (pinAt(i, IN_P_PIN)) return 1;
    if (pinAt(i, IN_M_PIN)) return -1;
    return 0;
}

struct BitDec { int value; int from; int to; };       // samples the bit's pulses cover
struct FrameDec {
    int start = -1, end = -1;                         // first pulse .. last bit's end
    std::vector<BitDec> bits;
    int frame = -1;                                   // -1: receiver gave up
};

// Run of `pin` high that contains or follows i: [a, b)
bool runAt(int i, uint pin, int limit, int& a, int& b) {
    int k = i;
    while (k < kSamples && k < i + limit && !pinAt(k, pin)) ++k;
    if (k >= kSamples || k >= i + limit) return false;
    a = k;
    while (k < kSamples && pinAt(k, pin)) ++k;
    b = k;
    return true;
}

FrameDec decodeFrame(int from) {
    FrameDec f;
    const uint plus = IN_P_PIN, minus = IN_M_PIN;
    int a, b;
    if (!runAt(from, plus, kSamples, a, b)) return f;                 // the first plus pulse
    f.start = a;
    // (a minus pulse just before it belongs to the frame too)
    for (int k = a - 1; k >= 0 && k > a - 12; --k) if (pinAt(k, minus)) f.start = k;
    while (f.start > 0 && pinAt(f.start - 1, minus)) --f.start;
    int i = b, value = 0;
    for (int n = 0; n < 11; ++n) {
        int pa, pb;
        if (!runAt(i, plus, kBitTimeout, pa, pb)) { f.end = i; return f; }
        const int at = pb + kSampleAfterPlus;
        if (at >= kSamples) { f.end = pb; return f; }
        const int v = pinAt(at, minus) ? 1 : 0;
        BitDec bit{ v, pa, pb };
        if (v) {                                       // minus follows plus
            int ma, mb;
            if (runAt(pb, minus, 12, ma, mb)) bit.to = mb;
        } else {                                       // minus came before plus
            for (int k = pa - 1; k >= 0 && k > pa - 12; --k) if (pinAt(k, minus)) bit.from = k;
            while (bit.from > 0 && pinAt(bit.from - 1, minus)) --bit.from;
        }
        f.bits.push_back(bit);
        value = (value << 1) | v;
        i = std::max(bit.to, pb);
    }
    f.frame = value;
    f.end = i;
    // The sync bit's extra pulse pair belongs to the first bit
    if (!f.bits.empty()) f.bits[0].from = f.start;
    return f;
}

std::vector<FrameDec> decodeAll() {
    std::vector<FrameDec> v;
    int i = 0;
    while (i < kSamples && v.size() < 64) {
        FrameDec f = decodeFrame(i);
        if (f.start < 0) break;
        v.push_back(f);
        i = std::max(f.end, f.start + 1);
    }
    return v;
}

void widths(uint gpio, int& n, float& mn, float& mx) {
    n = 0; mn = 1e9f; mx = 0;
    int start = -1;
    for (int i = 0; i < kSamples; ++i) {
        const bool h = pinAt(i, gpio);
        if (h && start < 0) start = i;
        if (!h && start >= 0) {
            const float w = static_cast<float>(i - start) * 1e6f / kSampleHz;
            if (w < mn) mn = w;
            if (w > mx) mx = w;
            ++n;
            start = -1;
        }
    }
    if (n == 0) mn = 0;
}

// ── Drawing ─────────────────────────────────────────────────────────────
DisplayDriver* d_ = nullptr;
constexpr std::uint16_t kYellow = 0xEDC0, kCyan = 0x563C, kBlack = 0x0000, kWhite = 0xFFFF;
constexpr std::uint16_t kGrey = 0x7BEF, kDark = 0x2104, kRed = 0xF800, kGreen = 0x07E0;
// Bit-cell bands: dark, but clearly lighter than the black background so
// they read as intended shading (the earlier 0x0841 was too close to
// black and looked like a drawing fault)
constexpr std::uint16_t kBand = 0x2965;
BitCells bitCells_ = BitCells::Bands;

bool visible() {
    return d_ != nullptr && plotterview_output() == DisplayOutput::Signals && !plotterview_isSplashVisible();
}

void text(int x, int y, const std::string& s, std::uint16_t fg, std::uint16_t bg = kBlack) {
    d_->txtSize(0);
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s.c_str());
}
void textBig(int x, int y, const std::string& s, std::uint16_t fg) {
    d_->txtSize(1);
    d_->txtColor(fg, kBlack);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s.c_str());
    d_->txtSize(0);
}
void centred(int cx, int y, const std::string& s, std::uint16_t fg) {
    text(cx - static_cast<int>(s.size()) * 4, y, s, fg);
}

// The waveform like in the HP-IL specification: one line, up for a plus
// pulse, down for a minus pulse. Samples [from, to) over [x, x + w), the
// zero line at y0, pulses `amp` pixels high, `thick` pixels wide.
void waveform(int x, int w, int y0, int amp, int from, int to, int thick, std::uint16_t col) {
    const int n = to - from;
    if (n <= 0) return;
    auto px = [&](int s) { return x + static_cast<int>(static_cast<long>(s - from) * w / n); };
    auto ylev = [&](int l) { return y0 - l * amp; };
    int runStart = from, lev = level(from);
    for (int s = from + 1; s <= to; ++s) {
        const int l = s < to ? level(s) : 99;
        if (l == lev) continue;
        const int x0 = px(runStart), x1 = px(s);
        d_->fillRect(x0, ylev(lev) - thick / 2, std::max(1, x1 - x0 + thick), thick, col);   // level
        if (s < to) {                                                   // edge to the next level
            const int ya = std::min(ylev(lev), ylev(l)), yb = std::max(ylev(lev), ylev(l));
            d_->fillRect(x1, ya - thick / 2, thick, yb - ya + thick, col);
        }
        runStart = s;
        lev = l;
    }
}

const char* frameClass(int f) {
    if ((f & 0x400) == 0) return "data or end (DOE)";
    switch ((f >> 8) & 7) {
        case 4: return "command (CMD)";
        case 5: return "ready (RDY)";
        default: return "identify (IDY)";
    }
}

void renderFull() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, kBlack);
    d_->fillRect(0, 0, SCREEN_MAX_X, 26, kYellow);
    text(8, 5, "HP-IL SIGNALS", kBlack, kYellow);
    char b[160];
    std::snprintf(b, sizeof(b), "HP-IL IN as the receiver sees it: up = GPIO%u, down = GPIO%u",
                  static_cast<unsigned>(IN_P_PIN), static_cast<unsigned>(IN_M_PIN));
    text(190, 5, b, kBlack, kYellow);

    if (!haveCapture_) {
        text(12, 200, "Waiting for traffic on HP-IL IN...", kGrey);
        text(12, SCREEN_MAX_Y - 24, "X: leave", kGrey);
        return;
    }
    const std::vector<FrameDec> frames = decodeAll();
    if (frames.empty()) {
        text(12, 200, "Something arrived, but no frame start (a pulse on the receiver's plus line) was found.", kRed);
        waveform(12, 1000, 300, 30, 0, kSamples, 1, kYellow);
        text(12, SCREEN_MAX_Y - 24, "A new capture every 0.5 s while traffic comes in.   X: leave", kGrey);
        return;
    }

    // ── The first frame, large, like the specification's drawing ─────────
    // The capture starts when a pulse has already begun, so the first frame
    // in it usually lacks the start of its first pulse -- show the first
    // complete one that has quiet line in front of it
    std::size_t pick = 0;
    for (std::size_t k = 0; k < frames.size(); ++k) {
        if (frames[k].start >= 16 && frames[k].frame >= 0) { pick = k; break; }
    }
    const FrameDec& f = frames[pick];
    // 1.5 µs of quiet line (the zero level) before the first pulse -- time 0
    // is where the frame starts going up or down. (Samples before the
    // capture count as quiet: level() is 0 there.)
    const int from = f.start - 12, to = std::min(kSamples, f.end + 8);
    const int X = 20, W = SCREEN_MAX_X - 40, Y0 = 170, AMP = 44;
    auto px = [&](int s) { return X + static_cast<int>(static_cast<long>(s - from) * W / (to - from)); };
    d_->fillRect(X, Y0, W, 1, kDark);                       // zero line
    // Bit cells: the value on top, the field name below. The cells are
    // marked as chosen in the menu (bitCells_): shaded bands behind every
    // other cell, thin lines between them, or not at all. Drawn before
    // the waveform, so the trace stays on top.
    static const char* kField[11] = { "C2", "C1", "C0", "D7", "D6", "D5", "D4", "D3", "D2", "D1", "D0" };
    // Cell borders lie midway between the last edge of one bit's pulses and
    // the first edge of the next, so every cell covers its share of the
    // quiet time too -- a "1" and a "0" get (about) the same width. The
    // outer borders mirror the half gap next to them.
    const std::size_t nBits = f.bits.size();
    std::vector<int> edge(nBits + 1, 0);
    for (std::size_t k = 1; k < nBits; ++k)
        edge[k] = (px(f.bits[k - 1].to) + px(f.bits[k].from)) / 2;
    if (nBits == 1) {
        edge[0] = px(f.bits[0].from) - 3;
        edge[1] = px(f.bits[0].to) + 3;
    } else if (nBits > 1) {
        edge[0] = px(f.bits[0].from) - (edge[1] - px(f.bits[0].to));
        edge[nBits] = px(f.bits[nBits - 1].to) + (px(f.bits[nBits - 1].from) - edge[nBits - 1]);
    }
    // ... but never outside the trace's own area
    if (nBits > 0) {
        edge[0] = std::max(edge[0], X);
        edge[nBits] = std::min(edge[nBits], X + W);
    }
    for (std::size_t k = 0; k < nBits; ++k) {
        const BitDec& bit = f.bits[k];
        const int x0 = px(bit.from) - 3, x1 = px(bit.to) + 3;
        if (bitCells_ == BitCells::Bands && k % 2 == 0) {
            d_->fillRect(edge[k], Y0 - AMP - 14, edge[k + 1] - edge[k], 2 * AMP + 28, kBand);
        } else if (bitCells_ == BitCells::Lines && k > 0) {
            d_->fillRect(edge[k], Y0 - AMP - 14, 1, 2 * AMP + 28, kDark);
        }
        const int cx = (x0 + x1) / 2;
        centred(cx, Y0 - AMP - 40, std::to_string(bit.value), kWhite);
        if (k == 0) centred(cx, Y0 - AMP - 60, "sync", kCyan);
        centred(cx, Y0 + AMP + 18, kField[k], kGrey);
    }
    waveform(X, W, Y0, AMP, from, to, 3, kYellow);
    // Control / data brackets
    if (f.bits.size() == 11) {
        const int yb = Y0 + AMP + 40;
        auto bracket = [&](int a, int bb, const char* name) {
            const int x0 = px(f.bits[static_cast<std::size_t>(a)].from), x1 = px(f.bits[static_cast<std::size_t>(bb)].to);
            d_->fillRect(x0, yb, x1 - x0, 1, kGrey);
            d_->fillRect(x0, yb - 5, 1, 5, kGrey);
            d_->fillRect(x1, yb - 5, 1, 5, kGrey);
            centred((x0 + x1) / 2, yb + 4, name, kGrey);
        };
        bracket(0, 2, "control");
        bracket(3, 10, "data");
    }
    // Time scale under it
    const float usPerPx = static_cast<float>(to - from) * 1e6f / kSampleHz / W;
    const int stepUs = usPerPx * 80 > 5 ? 10 : 5;
    for (int us = 0; ; us += stepUs) {
        const int x = px(f.start) + static_cast<int>(us / usPerPx);         // 0 = the first pulse
        if (x > X + W) break;
        d_->fillRect(x, Y0 + AMP + 72, 1, 6, kGrey);
        text(x + 3, Y0 + AMP + 68, std::to_string(us) + " us", 0x528A);
    }
    // What it is
    int y = Y0 + AMP + 100;
    if (f.frame >= 0) {
        char m[32];
        ilMnemonic(static_cast<IL_CMD_t>(f.frame), m);
        std::string mn(m);
        while (!mn.empty() && mn.back() == ' ') mn.pop_back();
        std::snprintf(b, sizeof(b), "%s", mn.c_str());
        textBig(20, y, b, kGreen);
        std::snprintf(b, sizeof(b), "frame 0x%03X  --  %s", f.frame, frameClass(f.frame));
        text(40 + static_cast<int>(mn.size()) * 16, y + 10, b, kWhite);
    } else {
        std::snprintf(b, sizeof(b), "The receiver gives up after %d of 11 bits -- no frame",
                      static_cast<int>(f.bits.size()));
        textBig(20, y, b, kRed);
    }
    y += 44;
    // Pulse widths and the frame's length
    int np, nm; float pmn, pmx, mmn, mmx;
    widths(IN_P_PIN, np, pmn, pmx);
    widths(IN_M_PIN, nm, mmn, mmx);
    std::snprintf(b, sizeof(b), "Pulses up %.2f-%.2f us, down %.2f-%.2f us wide   frame %.1f us long",
                  pmn, pmx, mmn, mmx, (f.end - f.start) * 1e6f / kSampleHz);
    text(20, y, b, kGrey);
    y += 26;

    // ── The whole capture, small, with the frames found in it ─────────────
    std::snprintf(b, sizeof(b), "The whole capture (%.1f ms, no. %lu): %d frames --",
                  kSamples * 1000.0f / kSampleHz, (unsigned long)captureNo_, static_cast<int>(frames.size()));
    text(20, y, b, kGrey);
    std::string list;
    for (const FrameDec& fr : frames) {
        char m[32];
        if (fr.frame >= 0) { ilMnemonic(static_cast<IL_CMD_t>(fr.frame), m); }
        else std::snprintf(m, sizeof(m), "??");
        std::string s(m);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (list.size() + s.size() > 70) { list += " ..."; break; }
        list += " " + s;
    }
    text(20 + static_cast<int>(std::strlen(b)) * 8, y, list, kWhite);
    y += 22;
    const int oy = y + 50;
    d_->fillRect(20, oy, W, 1, kDark);
    waveform(20, W, oy, 18, 0, kSamples, 1, kYellow);
    // Each frame's name above it (where there's room)
    int lastLabelEnd = -1000;
    for (const FrameDec& fr : frames) {
        const int fx = 20 + static_cast<int>(static_cast<long>(fr.start) * W / kSamples);
        char m[32];
        if (fr.frame >= 0) ilMnemonic(static_cast<IL_CMD_t>(fr.frame), m);
        else std::snprintf(m, sizeof(m), "??");
        std::string s(m);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (fx < lastLabelEnd + 8) continue;
        text(fx, oy - 44, s, fr.frame >= 0 ? kWhite : kRed);
        lastLabelEnd = fx + static_cast<int>(s.size()) * 8;
    }
    // Where the large frame is
    const int hx0 = 20 + static_cast<int>(static_cast<long>(std::max(0, from)) * W / kSamples);
    const int hx1 = 20 + static_cast<int>(static_cast<long>(to) * W / kSamples);
    d_->fillRect(hx0, oy + 24, std::max(2, hx1 - hx0), 3, kCyan);
    text(hx0, oy + 30, "^ shown above", kCyan);

    text(12, SCREEN_MAX_Y - 24, "A new capture every 0.5 s while traffic comes in.   X: leave", kGrey);
}

}  // namespace

void hpil_diag_init(DisplayDriver* display) {
    d_ = display;
    // The scope on PIO2 -- reading the pins only: no pio_gpio_init(), the
    // pins stay the receiver's (PIO0). Started only while the view is shown.
    scopeOffset_ = pio_add_program(kPio, &hpil_scope_program);
    smScope_ = pio_claim_unused_sm(kPio, true);
    pio_sm_config c = hpil_scope_program_get_default_config(scopeOffset_);
    sm_config_set_in_pins(&c, kScopeBase);
    sm_config_set_in_shift(&c, true, true, 32);               // right, autopush every 16 samples
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, static_cast<float>(clock_get_hz(clk_sys)) / (2.0f * kSampleHz));
    pio_sm_init(kPio, smScope_, scopeOffset_, &c);
    dma_ = dma_claim_unused_channel(true);
}

void hpil_diag_poll() {
    if (d_ == nullptr) return;
    // Only while the view is shown
    if (!visible()) {
        if (armed_) {                                  // left the view: stop
            dma_channel_abort(static_cast<uint>(dma_));
            pio_sm_set_enabled(kPio, smScope_, false);
            armed_ = false;
        }
        return;
    }
    if (armed_ && !dma_channel_is_busy(static_cast<uint>(dma_))) {
        pio_sm_set_enabled(kPio, smScope_, false);
        armed_ = false;
        haveCapture_ = true;
        ++captureNo_;
        hpil_diag_drawAll();
        nextArm_ = make_timeout_time_ms(500);
    }
    if (!armed_ && time_reached(nextArm_)) arm();
}

void hpil_diag_drawAll() {
    if (d_ == nullptr) return;
#ifdef DISPLAY_7INCH
    // Off-screen, then copied in one go (no flicker), like the loop map
    const std::uint32_t prevAddr = d_->canvasAddr();
    const std::uint16_t prevStride = d_->canvasStride();
    d_->beginLayerDraw(LT7683::kBteLayer3Addr);
    renderFull();
    if (prevAddr == 0) d_->endOverlayDraw();
    else               d_->beginLayerDraw(prevAddr, prevStride);
    d_->copyLayerToPanel(LT7683::kBteLayer3Addr, 0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
#else
    renderFull();
#endif
}

void hpil_diag_setBitCells(BitCells style) {
    if (static_cast<std::uint8_t>(style) > static_cast<std::uint8_t>(BitCells::None)) style = BitCells::Bands;
    bitCells_ = style;
#ifdef DISPLAY_7INCH
    // Off-screen and copied under the menu's PIP overlay: safe to redraw
    // now. (5": the next capture, or closing the menu, redraws the view.)
    if (visible()) hpil_diag_drawAll();
#endif
}

BitCells hpil_diag_bitCells() { return bitCells_; }

const char* hpil_diag_bitCellsName(BitCells style) {
    switch (style) {
        case BitCells::Lines: return "Lines";
        case BitCells::None:  return "None";
        default:              return "Bands";
    }
}

}  // namespace hipi
