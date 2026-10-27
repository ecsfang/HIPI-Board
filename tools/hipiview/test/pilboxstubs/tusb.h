// Host stand-in for TinyUSB's CDC API, for pilbox_test.cpp: the PC side
// (pyILPER) is a pair of byte queues the test fills and reads
#pragma once
#include <cstdint>
#include <deque>
#include "pico/time.h"
namespace pilstub {
inline bool connected = true;
inline std::deque<std::uint8_t> fromPc, toPc;
inline unsigned long long tasks = 0;      // tud_task() calls (the waiting loops)
inline void (*onTask)() = nullptr;        // the "PC" may act while HIPI waits
}
inline bool tud_cdc_n_connected(std::uint8_t) { return pilstub::connected; }
inline std::uint32_t tud_cdc_n_available(std::uint8_t) { return static_cast<std::uint32_t>(pilstub::fromPc.size()); }
inline std::int32_t tud_cdc_n_read_char(std::uint8_t) {
    if (pilstub::fromPc.empty()) return -1;
    const std::uint8_t b = pilstub::fromPc.front();
    pilstub::fromPc.pop_front();
    return b;
}
inline bool tud_cdc_n_peek(std::uint8_t, std::uint8_t* b) {
    if (pilstub::fromPc.empty()) return false;
    *b = pilstub::fromPc.front();
    return true;
}
inline std::uint32_t tud_cdc_n_read(std::uint8_t itf, void* buf, std::uint32_t n) {
    std::uint32_t k = 0;
    auto* p = static_cast<std::uint8_t*>(buf);
    while (k < n && !pilstub::fromPc.empty()) { p[k++] = pilstub::fromPc.front(); pilstub::fromPc.pop_front(); }
    (void)itf;
    return k;
}
inline void tusb_init() {}
inline void tud_cdc_n_read_flush(std::uint8_t) { pilstub::fromPc.clear(); }
inline std::uint32_t tud_cdc_n_write_char(std::uint8_t, char c) { pilstub::toPc.push_back(static_cast<std::uint8_t>(c)); return 1; }
inline std::uint32_t tud_cdc_n_write(std::uint8_t, const void* buf, std::uint32_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    pilstub::toPc.insert(pilstub::toPc.end(), p, p + n);
    return n;
}
inline std::uint32_t tud_cdc_n_write_flush(std::uint8_t) { return 0; }
inline bool tud_cdc_n_write_clear(std::uint8_t) { return true; }
inline void tud_task() {
    ++pilstub::tasks;
    pilClockUs() += 10;
    if (pilstub::onTask) pilstub::onTask();
}
