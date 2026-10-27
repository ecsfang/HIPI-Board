#pragma once
#include <cstdio>
#undef LOGF   // (hpil.h may have the firmware's already)
#define LOGF(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)
