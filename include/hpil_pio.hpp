// src/hpil_pio.hpp
#pragma once

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "hpil.pio.h"

class HpIlLoop {
public:
    HpIlLoop(uint minus_in_pin, uint plus_in_pin,
             uint minus_out_pin, uint plus_out_pin)
        : minus_in_pin_(minus_in_pin),
          plus_in_pin_(plus_in_pin),
          minus_out_pin_(minus_out_pin),
          plus_out_pin_(plus_out_pin) {
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
        return true;
    }

    // Skriv ett 32-bitars ord till TX-FIFO (icke-blockerande).
    // Returnerar true om data skickades, false om FIFO är full.
    bool try_write(uint32_t word) {
        if (pio_sm_is_tx_fifo_full(pio0, sm_tx_)) {
            return false;
        }
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

        // RX
        pio_sm_config rx_cfg = frame_rx_program_get_default_config(rx_offset);
        sm_config_set_in_pins(&rx_cfg, minus_in_pin_);
        sm_config_set_jmp_pin(&rx_cfg, plus_in_pin_);
        sm_config_set_in_shift(&rx_cfg, true, false, 32);
        sm_config_set_fifo_join(&rx_cfg, PIO_FIFO_JOIN_RX);
        pio_sm_init(pio0, sm_rx_, rx_offset, &rx_cfg);

        // TX
        pio_sm_config tx_cfg = frame_tx_program_get_default_config(tx_offset);
        sm_config_set_out_pins(&tx_cfg, minus_out_pin_, 1);
        sm_config_set_sideset_pins(&tx_cfg, plus_out_pin_);
        sm_config_set_out_shift(&tx_cfg, true, false, 32);
        sm_config_set_fifo_join(&tx_cfg, PIO_FIFO_JOIN_TX);
        pio_sm_init(pio0, sm_tx_, tx_offset, &tx_cfg);

        // Pin directions
        pio_gpio_init(pio0, minus_in_pin_);
        pio_gpio_init(pio0, plus_in_pin_);
        pio_gpio_init(pio0, minus_out_pin_);
        pio_gpio_init(pio0, plus_out_pin_);

        SetPinDriveStrength(minus_out_pin_, 12); // Set drive strenth to 12mA
        SetPinDriveStrength(plus_out_pin_, 12); // Set drive strenth to 12mA

        gpio_set_pulls(minus_in_pin_, false, true);
        gpio_set_pulls(plus_in_pin_, false, true);

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


// I hpil_pio.hpp, lägg till:

#include "hpil_debug.pio.h"

class HpIlDebug {
public:
    HpIlDebug(uint minus_in_pin, uint plus_in_pin) {
        sm_ = pio_claim_unused_sm(pio0, true);
        uint offset = pio_add_program(pio0, &hpil_debug_program);
        
        pio_sm_config cfg = hpil_debug_program_get_default_config(offset);
        sm_config_set_in_pins(&cfg, minus_in_pin);
        sm_config_set_jmp_pin(&cfg, plus_in_pin);
        sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);
        pio_sm_init(pio0, sm_, offset, &cfg);
        
        pio_gpio_init(pio0, minus_in_pin);
        pio_gpio_init(pio0, plus_in_pin);
        gpio_set_pulls(minus_in_pin, false, true);
        gpio_set_pulls(plus_in_pin, false, true);
        
        pio_sm_set_enabled(pio0, sm_, true);
    }
    
    // Läs ett ord (icke-blockerande)
    bool read(uint32_t& out) {
        if (pio_sm_is_rx_fifo_empty(pio0, sm_)) return false;
        out = pio_sm_get(pio0, sm_);
        return true;
    }
    
    uint sm() const { return sm_; }
    
private:
    uint sm_;
};

#if 1
#include "hpil_debug_1hz.pio.h"
// För Pico 2 W (inbyggd LED på CYW43):
//#include "pico/cyw43_arch.h"

class HpIlDebug1Hz {
public:
    HpIlDebug1Hz(uint out_pin) {
        sm_ = pio_claim_unused_sm(pio0, true);
        uint offset = pio_add_program(pio0, &hpil_debug_1hz_program);
        
        pio_sm_config cfg = hpil_debug_1hz_program_get_default_config(offset);
        sm_config_set_out_pins(&cfg, out_pin, 1);
        sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
        sm_config_set_clkdiv(&cfg, 250.0f);
        
        pio_sm_init(pio0, sm_, offset, &cfg);
        pio_gpio_init(pio0, out_pin);
        // gpio_set_dir behövs inte — PIO:n sätter direction via 'set pindirs'
        
        pio_sm_set_enabled(pio0, sm_, true);
    }
    
    uint sm() const { return sm_; }

private:
    uint sm_;
};



#endif