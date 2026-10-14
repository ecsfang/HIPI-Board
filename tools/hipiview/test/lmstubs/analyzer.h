// Host stand-in for the analyzer (loopmap_test): the frames the test puts
// on the "loop", read back by the loop map like from the real capture
#pragma once
#include "hpil.h"
#include "pico/time.h"
#include <cstdint>
#include <vector>
#define HIPI_ANALYZER 1
namespace hipi {
struct StubFrame { IL_CMD_t in, out; std::uint64_t t; };
inline std::vector<StubFrame>& analyzerStubCapture() { static std::vector<StubFrame> v; return v; }
// A frame now: in, and what left HIPI (out = in: passed on unchanged)
inline void analyzer_capture(IL_CMD_t in, IL_CMD_t out) { analyzerStubCapture().push_back({ in, out, time_us_64() }); }
inline void analyzer_clear() { analyzerStubCapture().clear(); }
inline bool analyzer_nextFrame(std::uint32_t& cursor, IL_CMD_t& in, IL_CMD_t& out, std::uint64_t& t) {
    auto& v = analyzerStubCapture();
    if (cursor >= v.size()) return false;
    in = v[cursor].in; out = v[cursor].out; t = v[cursor].t;
    ++cursor;
    return true;
}
}
