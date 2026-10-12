// Host stand-in for the analyzer (view tests): records what it's given
#pragma once
#include "hpil.h"
#include <cstdint>
#define HIPI_ANALYZER 1
namespace hipi {
inline std::uint32_t& analyzerStubFrames() { static std::uint32_t n = 0; return n; }
inline void analyzer_redrawAll() {}
inline void analyzer_clear() { analyzerStubFrames() = 0; }
inline void analyzer_capture(IL_CMD_t, IL_CMD_t) { ++analyzerStubFrames(); }
}
