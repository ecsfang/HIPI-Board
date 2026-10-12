#pragma once
#include "hardware/pio.h"
static const pio_program WS2812B_program = { 0 };
static inline void WS2812B_program_init(PIO, uint, uint, uint, float, uint) {}
