#pragma once
#include <cstdio>
#define LOGF(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)
