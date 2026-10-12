#pragma once
namespace hipi {
inline void loopmap_drawAll() {}
inline int& loopmapStubResets() { static int n = 0; return n; }
inline void loopmap_reset() { ++loopmapStubResets(); }
}
