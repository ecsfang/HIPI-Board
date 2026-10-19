#define MODULE "SIGNALS"
#include "hpil_diag.h"
#include "hpil_signals.h"
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
using signals::kScopeBase;                    // GPIO12 = bit 0, GPIO13 = bit 1 of each sample
using signals::kWords;
using signals::kSampleHz;
static_assert(signals::kPinPlus == IN_P_PIN && signals::kPinMinus == IN_M_PIN,
              "hpil_signals.h: the receiver's plus/minus pins");

PIO const kPio = pio2;
uint smScope_ = 0;
uint scopeOffset_ = 0;
int dma_ = -1;

// ── Scope ───────────────────────────────────────────────────────────────
std::uint32_t buf_[kWords];                           // 16 samples a word: ~4.1 ms
bool armed_ = false;
absolute_time_t nextArm_ = nil_time;
std::uint32_t captureNo_ = 0;

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

DisplayDriver* d_ = nullptr;
BitCells bitCells_ = BitCells::Bands;
bool live_ = false;                                   // a real capture in buf_ (else: the example)

bool visible() {
    return d_ != nullptr && plotterview_output() == DisplayOutput::Signals && !plotterview_isSplashVisible();
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
        live_ = true;
        ++captureNo_;
        hpil_diag_drawAll();
        nextArm_ = make_timeout_time_ms(500);
    }
    if (!armed_ && time_reached(nextArm_)) arm();
}

void hpil_diag_drawAll() {
    if (d_ == nullptr) return;
    // No traffic yet: the example (hpil_signals.h)
    signals::draw(d_, live_ ? buf_ : nullptr, captureNo_, bitCells_);
}

void hpil_diag_setBitCells(BitCells style) {
    if (static_cast<std::uint8_t>(style) > static_cast<std::uint8_t>(BitCells::None)) style = BitCells::Bands;
    bitCells_ = style;
    // Off-screen and copied under the menu's PIP overlay: safe to redraw
    // now.
    if (visible()) hpil_diag_drawAll();
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
