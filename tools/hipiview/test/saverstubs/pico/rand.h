// Host stand-in for the Pico SDK's random numbers (saver_test): repeatable
#pragma once
#include <cstdint>
#include <random>
inline std::uint32_t get_rand_32() { static std::mt19937 g(1234); return g(); }
