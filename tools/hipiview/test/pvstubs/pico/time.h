// Host stand-in for the Pico SDK clock (printer_view_test): the test moves it
#pragma once
#include <cstdint>
typedef std::uint64_t absolute_time_t;
extern std::uint64_t fakeNowUs;
static constexpr absolute_time_t nil_time = 0;
inline absolute_time_t get_absolute_time() { return fakeNowUs; }
inline absolute_time_t make_timeout_time_ms(std::uint32_t ms) { return fakeNowUs + ms * 1000ull; }
inline absolute_time_t make_timeout_time_us(std::uint64_t us) { return fakeNowUs + us; }
inline bool time_reached(absolute_time_t t) { return fakeNowUs >= t; }
inline std::int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b) {
    return static_cast<std::int64_t>(b) - static_cast<std::int64_t>(a);
}
inline std::uint32_t to_ms_since_boot(absolute_time_t t) { return static_cast<std::uint32_t>(t / 1000); }
inline void sleep_ms(std::uint32_t ms) { fakeNowUs += ms * 1000ull; }
inline void sleep_us(std::uint64_t us) { fakeNowUs += us; }
inline std::uint64_t time_us_64() { return fakeNowUs; }
