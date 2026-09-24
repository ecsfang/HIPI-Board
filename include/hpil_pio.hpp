// src/hpil_pio.hpp
#pragma once

#include <stdio.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"
#include "pico/time.h"
#include "hpil.pio.h"
#include "usb_serial.h"

#define IN_M_PIN  13
#define IN_P_PIN  12
#define OUT_M_PIN 15
#define OUT_P_PIN 14

#define WORD_SHIFT  0

class HpIlLoop {
public:
    HpIlLoop(uint minus_in_pin, uint plus_in_pin,
             uint minus_out_pin, uint plus_out_pin)
        : minus_in_pin_(minus_in_pin),
          plus_in_pin_(plus_in_pin),
          minus_out_pin_(minus_out_pin),
          plus_out_pin_(plus_out_pin) {
/*            if (plus_out_pin_ != minus_out_pin_ + 1) {
                LOGF("ERROR: TX pins not adjacent: minus=%u plus=%u (expected plus=minus+1)\n",
                    minus_out_pin_, plus_out_pin_);
                while (true) tight_loop_contents();  // halt visibly
            }
            if (plus_in_pin_ != minus_in_pin_ + 1) {
                LOGF("ERROR: RX pins not adjacent: minus=%u plus=%u\n",
                    minus_in_pin_, plus_in_pin_);
                while (true) tight_loop_contents();
            }
            if (minus_out_pin_ == minus_in_pin_) {
                LOGF("ERROR: TX and RX share pin: %u\n", minus_in_pin_);
                while (true) tight_loop_contents();
            }**/
            init();
    }

    ~HpIlLoop() {
        stop();
    }

    void restart() {
        stop();
        init();
    }

    void stop() {
        pio_sm_set_enabled(pio0, sm_rx_, false);
        pio_sm_set_enabled(pio0, sm_tx_, false);
    }

    // ==========================================================
    // PUBLIC API: read from RX, write to TX
    // ==========================================================

    // Read a 32-bit word from the RX FIFO (non-blocking).
    // Returns true if data was found, false if the FIFO is empty.
    bool try_read(uint32_t& out_word) {
        if (pio_sm_is_rx_fifo_empty(pio0, sm_rx_)) {
            return false;
        }
        out_word = pio_sm_get(pio0, sm_rx_);
        out_word >>= WORD_SHIFT;
        return true;
    }

    // Read a 32-bit word from the RX FIFO (blocking for at most timeout_ms).
    // Returns true if data was found.
    bool read(uint32_t& out_word, uint32_t timeout_ms = 1000) {
        absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
        
        // Wait until the FIFO is not empty
        while (pio_sm_is_rx_fifo_empty(pio0, sm_rx_)) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                return false;  // timeout — no data
            }
            tight_loop_contents();
        }
        
        // FIFO has data — read it
        out_word = pio_sm_get(pio0, sm_rx_);
        out_word >>= WORD_SHIFT;
        return true;
    }

    // Write a 32-bit word to the TX FIFO (non-blocking).
    // Returns true if data was sent, false if the FIFO is full.
    bool try_write(uint32_t word) {
        if (pio_sm_is_tx_fifo_full(pio0, sm_tx_)) {
            return false;
        }
        word <<= WORD_SHIFT;
        pio_sm_put(pio0, sm_tx_, word);
        return true;
    }

    // Write a 32-bit word to the TX FIFO (blocking for at most timeout_ms).
    bool write(uint32_t word, uint32_t timeout_ms = 1000) {
        absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
        while (pio_sm_is_tx_fifo_full(pio0, sm_tx_)) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                return false;
            }
            tight_loop_contents();
        }
        word <<= WORD_SHIFT;
        pio_sm_put(pio0, sm_tx_, word);
        return true;
    }

    // ==========================================================
    // Convenience for testing
    // ==========================================================

    // Send a word and read the reply (echo loopback).
    // Useful for testing without a real HP-IL controller.
    uint32_t roundtrip(uint32_t word, uint32_t timeout_ms = 1000) {
        write(word, timeout_ms);
        uint32_t rx;
        read(rx, timeout_ms);
        return rx;
    }
    // Aliases with simpler names
    bool sendFrame(uint32_t word) {
        return try_write(word);
    }

    bool receiveFrame(uint32_t& out_word) {
        return try_read(out_word);
    }

    // Access for debugging
    uint sm_rx() const { return sm_rx_; }
    uint sm_tx() const { return sm_tx_; }

private:
    void init() {
        // Explicit hardware reset of the whole PIO0 block first, before
        // touching anything else. RP2040/RP2350's PIO peripherals are
        // NOT reset by a software/watchdog reset -- only by a full power
        // cycle (or this explicit RESETS-block reset) -- so any state
        // left over from a previous run (a crash, a watchdog reboot, or
        // even just a debugger-triggered restart) can leave a state
        // machine mid-program, its FIFOs non-empty, or its GPIO function
        // assignments stale, none of which pio_sm_init() below alone
        // guarantees is cleared (it resets the state machine's OWN
        // program counter/config, not the block's overall hardware
        // state). Matches "sometimes HP-IL just doesn't come up, but a
        // restart fixes it" exactly -- a power-on boot always starts
        // clean, but a soft reset doesn't reliably.
        reset_block(RESETS_RESET_PIO0_BITS);
        unreset_block_wait(RESETS_RESET_PIO0_BITS);

        sm_rx_ = pio_claim_unused_sm(pio0, true);
        sm_tx_ = pio_claim_unused_sm(pio0, true);
        uint rx_offset = pio_add_program(pio0, &frame_rx_program);
        uint tx_offset = pio_add_program(pio0, &frame_tx_program);

        // Platform-independent PIO frequency (125 MHz on RP2040, 150 MHz on RP2350)
        const float pio_freq = 4000000.0f;
        const float clkdiv = (float)clock_get_hz(clk_sys) / pio_freq;

        // ============ RX ============
        pio_sm_set_enabled(pio0, sm_rx_, false);
        pio_sm_set_enabled(pio0, sm_tx_, false);

        pio_sm_config rx_cfg = frame_rx_program_get_default_config(rx_offset);
        sm_config_set_in_pins(&rx_cfg, minus_in_pin_);
        sm_config_set_jmp_pin(&rx_cfg, plus_in_pin_);
        sm_config_set_in_shift(&rx_cfg, false, false, 32);
        sm_config_set_fifo_join(&rx_cfg, PIO_FIFO_JOIN_RX);
        sm_config_set_clkdiv(&rx_cfg, clkdiv);
        sm_config_set_wrap(&rx_cfg, rx_offset + frame_rx_wrap_target, rx_offset + frame_rx_wrap);
        pio_sm_init(pio0, sm_rx_, rx_offset, &rx_cfg);
        //pio_sm_set_wrap(pio0, sm_rx_, 0, 10);

        pio_gpio_init(pio0, minus_in_pin_);
        pio_gpio_init(pio0, plus_in_pin_);
        // 2 pins, input: minus_in_pin and plus_in_pin (minus=base, plus=base+1)
        pio_sm_set_consecutive_pindirs(pio0, sm_rx_, minus_in_pin_,  2, false);
        gpio_set_pulls(minus_in_pin_, false, false);
        gpio_set_pulls(plus_in_pin_,  false, false);

        // ============ TX ============
        pio_sm_config tx_cfg = frame_tx_program_get_default_config(tx_offset);
        sm_config_set_out_pins(&tx_cfg, plus_out_pin_, 1);
        sm_config_set_sideset_pins(&tx_cfg, plus_out_pin_);
        sm_config_set_out_shift(&tx_cfg, false, false, 32);
        sm_config_set_fifo_join(&tx_cfg, PIO_FIFO_JOIN_TX);
        sm_config_set_clkdiv(&tx_cfg, clkdiv);
        sm_config_set_wrap(&tx_cfg, tx_offset + frame_tx_wrap_target, tx_offset + frame_tx_wrap);
        pio_sm_init(pio0, sm_tx_, tx_offset, &tx_cfg);
        pio_sm_set_wrap(pio0, sm_tx_, tx_offset, tx_offset + frame_tx_wrap);

        pio_gpio_init(pio0, minus_out_pin_);
        pio_gpio_init(pio0, plus_out_pin_);
        pio_sm_set_consecutive_pindirs(pio0, sm_tx_, plus_out_pin_, 2, true);

        SetPinDriveStrength(plus_out_pin_, 12);
        SetPinDriveStrength(minus_out_pin_,  12);

        // Start both SMs — RX starts listening, TX pulls immediately and blocks
        // until the first write() sends data.
        pio_sm_set_enabled(pio0, sm_rx_, true);
        pio_sm_set_enabled(pio0, sm_tx_, true);
    }

    void SetPinDriveStrength(uint pin, uint mA) {
        // gpio_set_drive_strength() is the pico-sdk API for this
        // The value type is named 'gpio_drive_strength' (not _t)
        
        if (mA <= 2) {
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_2MA);
        } else if (mA <= 4) {
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_4MA);
        } else if (mA <= 8) {
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
        } else {
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        }
    }
    uint minus_in_pin_;
    uint plus_in_pin_;
    uint minus_out_pin_;
    uint plus_out_pin_;
    uint sm_rx_;
    uint sm_tx_;
};
