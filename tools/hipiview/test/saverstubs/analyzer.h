// Host stand-in for the analyzer (saver_test): no frames for the rain
#pragma once
#include <cstddef>
namespace hipi {
inline bool analyzer_nextFrameText(char*, std::size_t) { return false; }
}
