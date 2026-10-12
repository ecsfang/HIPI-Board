// Host stand-in for TinyUSB, as far as HIPI's usb_serial.h needs it: the
// debug console (CDC 0) goes to stdout
#pragma once
#include <cstdint>
#include <cstdio>
inline bool tusb_init() { return true; }
inline void tud_task() {}
inline bool tud_mounted() { return true; }
inline bool tud_cdc_n_connected(std::uint8_t) { return true; }
inline std::uint32_t tud_cdc_n_available(std::uint8_t) { return 0; }
inline std::uint32_t tud_cdc_n_read(std::uint8_t, void*, std::uint32_t) { return 0; }
inline std::uint32_t tud_cdc_n_write(std::uint8_t itf, const void* buf, std::uint32_t n) {
    if (itf == 0) std::fwrite(buf, 1, n, stdout);
    return n;
}
inline std::uint32_t tud_cdc_n_write_flush(std::uint8_t) { return 0; }
