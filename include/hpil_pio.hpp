// src/hpil_pio.hpp
#pragma once

#include <stdio.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "hpil.pio.h"

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
                printf("ERROR: TX pins not adjacent: minus=%u plus=%u (expected plus=minus+1)\n",
                    minus_out_pin_, plus_out_pin_);
                while (true) tight_loop_contents();  // stoppa tydligt
            }
            if (plus_in_pin_ != minus_in_pin_ + 1) {
                printf("ERROR: RX pins not adjacent: minus=%u plus=%u\n",
                    minus_in_pin_, plus_in_pin_);
                while (true) tight_loop_contents();
            }
            if (minus_out_pin_ == minus_in_pin_) {
                printf("ERROR: TX and RX share pin: %u\n", minus_in_pin_);
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
    // PUBLIC API: läs från RX, skriv till TX
    // ==========================================================

    // Läs ett 32-bitars ord från RX-FIFO (icke-blockerande).
    // Returnerar true om data hittades, false om FIFO är tom.
    bool try_read(uint32_t& out_word) {
        if (pio_sm_is_rx_fifo_empty(pio0, sm_rx_)) {
            return false;
        }
        out_word = pio_sm_get(pio0, sm_rx_);
        out_word >>= WORD_SHIFT;
        return true;
    }

    // Läs ett 32-bitars ord från RX-FIFO (blockerande i max timeout_ms).
    // Returnerar true om data hittades.
    bool read(uint32_t& out_word, uint32_t timeout_ms = 1000) {
        absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
        
        // Vänta tills FIFO inte är tom
        while (pio_sm_is_rx_fifo_empty(pio0, sm_rx_)) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                return false;  // timeout — ingen data
            }
            tight_loop_contents();
        }
        
        // FIFO har data — läs
        out_word = pio_sm_get(pio0, sm_rx_);
        out_word >>= WORD_SHIFT;
        return true;
    }

    // Skriv ett 32-bitars ord till TX-FIFO (icke-blockerande).
    // Returnerar true om data skickades, false om FIFO är full.
    bool try_write(uint32_t word) {
        if (pio_sm_is_tx_fifo_full(pio0, sm_tx_)) {
            return false;
        }
        word <<= WORD_SHIFT;
        pio_sm_put(pio0, sm_tx_, word);
        return true;
    }

    // Skriv ett 32-bitars ord till TX-FIFO (blockerande i max timeout_ms).
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
    // Convenience för testning
    // ==========================================================

    // Skicka ett ord och läs svaret (echo-loopback).
    // Användbart för test utan en riktig HP-IL-kontroller.
    uint32_t roundtrip(uint32_t word, uint32_t timeout_ms = 1000) {
        write(word, timeout_ms);
        uint32_t rx;
        read(rx, timeout_ms);
        return rx;
    }
    // Alias för enklare namn
    bool sendFrame(uint32_t word) {
        return try_write(word);
    }

    bool receiveFrame(uint32_t& out_word) {
        return try_read(out_word);
    }

    // Access för debugging
    uint sm_rx() const { return sm_rx_; }
    uint sm_tx() const { return sm_tx_; }

private:
    void init() {
        sm_rx_ = pio_claim_unused_sm(pio0, true);
        sm_tx_ = pio_claim_unused_sm(pio0, true);
        uint rx_offset = pio_add_program(pio0, &frame_rx_program);
        uint tx_offset = pio_add_program(pio0, &frame_tx_program);

        // Plattformsoberoende PIO-frekvens (125 MHz på RP2040, 150 MHz på RP2350)
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
        // 2 pinnar, input: minus_in_pin och plus_in_pin (minus=base, plus=base+1)
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

        // Starta båda SM:er — RX börjar lyssna, TX pull:ar direkt och blockerar
        // tills första write() skickar data.
        pio_sm_set_enabled(pio0, sm_rx_, true);
        pio_sm_set_enabled(pio0, sm_tx_, true);
    }

    void SetPinDriveStrength(uint pin, uint mA) {
        // gpio_set_drive_strength() är pico-SDK:s API för detta
        // Värdetypen heter 'gpio_drive_strength' (inte _t)
        
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
