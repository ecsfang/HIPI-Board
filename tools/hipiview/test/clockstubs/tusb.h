// tusb.h for clock_view_test: no console input
#pragma once
#include <cstdint>
inline void tud_task() {}
inline bool tud_cdc_n_connected(std::uint8_t) { return false; }
inline std::uint32_t tud_cdc_n_available(std::uint8_t) { return 0; }
inline std::int32_t tud_cdc_n_read_char(std::uint8_t) { return -1; }
inline std::uint32_t tud_cdc_n_write(std::uint8_t, const void*, std::uint32_t n) { return n; }
inline std::uint32_t tud_cdc_n_write_char(std::uint8_t, char) { return 1; }
inline std::uint32_t tud_cdc_n_write_flush(std::uint8_t) { return 0; }
inline std::uint32_t tud_cdc_n_write_available(std::uint8_t) { return 4096; }
inline void tusb_init() {}
inline std::uint32_t tud_cdc_n_read(std::uint8_t, void*, std::uint32_t) { return 0; }
