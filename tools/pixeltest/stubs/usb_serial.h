// Host stand-in: logging to stdout
#pragma once
#include <cstdio>
extern bool bTrace, bExtTrace;
#define MLOGF(...) do { std::printf("[" MODULE "] " __VA_ARGS__); std::printf("\n"); } while (0)
#define MTRC_LOGF(...) do { if (bTrace) MLOGF(__VA_ARGS__); } while (0)
#define MDBG_LOGF(...) do { if (bExtTrace) MLOGF(__VA_ARGS__); } while (0)
