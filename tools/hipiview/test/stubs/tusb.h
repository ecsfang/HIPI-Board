// Host stand-in for TinyUSB's CDC device API (link_sim.cpp implements it)
#pragma once
#include <cstdint>
bool tud_cdc_n_connected(std::uint8_t itf);
std::uint32_t tud_cdc_n_available(std::uint8_t itf);
std::uint32_t tud_cdc_n_read(std::uint8_t itf, void* buf, std::uint32_t n);
std::uint32_t tud_cdc_n_write_available(std::uint8_t itf);
std::uint32_t tud_cdc_n_write(std::uint8_t itf, const void* buf, std::uint32_t n);
std::uint32_t tud_cdc_n_write_flush(std::uint8_t itf);
bool tud_cdc_n_write_clear(std::uint8_t itf);
void tud_task();
