// src/hpil_pio.hpp
#pragma once

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "frame.pio.h"

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
        // Stoppa, ladda om, starta
        stop();
        init();
    }

    void stop() {
        pio_sm_set_enabled(pio0, sm_rx_, false);
        pio_sm_set_enabled(pio0, sm_tx_, false);
    }

private:
    void init() {
        // 1. Konfigurera PIO state machines
        sm_rx_ = pio_claim_unused_sm(pio0, true);
        sm_tx_ = pio_claim_unused_sm(pio0, true);

        // 2. Ladda PIO-program
        uint rx_offset = pio_add_program(pio0, &frame_rx_program);
        uint tx_offset = pio_add_program(pio0, &frame_tx_program);

        // Konfigurera RX
pio_sm_config rx_cfg = frame_rx_program_get_default_config(rx_offset);
sm_config_set_in_shift(&rx_cfg, true /* left */, false /* no autopush */, 32);
sm_config_set_in_pins(&rx_cfg, minus_in_pin);
sm_config_set_jmp_pin(&rx_cfg, plus_in_pin);

// Konfigurera TX
pio_sm_config tx_cfg = frame_tx_program_get_default_config(tx_offset);
sm_config_set_out_shift(&tx_cfg, true /* left */, false /* no autopush */, 32);
sm_config_set_out_pins(&tx_cfg, minus_out_pin, 1);
sm_config_set_sideset_pins(&tx_cfg, plus_out_pin);
// sideset_init styrs via SM pin initial state — görs i koden

//        // 3. Konfigurera RX state machine
//        pio_sm_config rx_cfg = frame_rx_program_get_default_config(rx_offset);
//        sm_config_set_in_pins(&rx_cfg, minus_in_pin_);      // in_base
//        sm_config_set_jmp_pin(&rx_cfg, plus_in_pin_);
//        sm_config_set_in_shift(&rx_cfg, true, false, 32);   // shift_left, no autopush
//        sm_config_set_fifo_join(&rx_cfg, PIO_FIFO_JOIN_RX);
//        pio_sm_init(pio0, sm_rx_, rx_offset, &rx_cfg);
//
//        // 4. Konfigurera TX state machine
//        pio_sm_config tx_cfg = frame_tx_program_get_default_config(tx_offset);
//        sm_config_set_out_pins(&tx_cfg, minus_out_pin_, 1);  // 1 pin
//        sm_config_set_sideset_pins(&tx_cfg, plus_out_pin_);
//        sm_config_set_out_shift(&tx_cfg, true, false, 32);
//        sm_config_set_fifo_join(&tx_cfg, PIO_FIFO_JOIN_TX);
//        pio_sm_init(pio0, sm_tx_, tx_offset, &tx_cfg);

        // 5. Sätt pin directions
        pio_gpio_init(pio0, minus_in_pin_);
        pio_gpio_init(pio0, plus_in_pin_);
        pio_gpio_init(pio0, minus_out_pin_);
        pio_gpio_init(pio0, plus_out_pin_);
        gpio_set_pulls(minus_in_pin_, false, true);   // pull-down
        gpio_set_pulls(plus_in_pin_, false, true);

        // 6. Starta state machines
        pio_sm_set_enabled(pio0, sm_rx_, true);
        pio_sm_set_enabled(pio0, sm_tx_, true);
    }

    uint minus_in_pin_;
    uint plus_in_pin_;
    uint minus_out_pin_;
    uint plus_out_pin_;
    uint sm_rx_;
    uint sm_tx_;
};
