#pragma once
#include <cstdint>
typedef std::uint64_t absolute_time_t;
absolute_time_t get_absolute_time();
inline absolute_time_t make_timeout_time_us(std::uint64_t us) { return get_absolute_time() + us; }
inline absolute_time_t make_timeout_time_ms(std::uint32_t ms) { return get_absolute_time() + ms * 1000ull; }
inline bool time_reached(absolute_time_t t) { return get_absolute_time() >= t; }
inline std::int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
}
