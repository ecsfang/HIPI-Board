// Host stand-in: PIO blocks; what the strip sends is recorded
#pragma once
#include "pico/types.h"
#include <vector>
struct pio_hw_t { int id; };
typedef pio_hw_t* PIO;
extern pio_hw_t g_pio0, g_pio1;
#define pio0 (&g_pio0)
#define pio1 (&g_pio1)
struct pio_program { int dummy; };
extern std::vector<uint32_t> g_wire;
inline void pio_sm_put_blocking(PIO, uint, uint32_t v) { g_wire.push_back(v); }
inline uint pio_add_program(PIO, const pio_program*) { return 0; }
