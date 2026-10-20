#pragma once
namespace hipi {
inline void loopmap_drawAll() {}
inline int& loopmapStubResets() { static int n = 0; return n; }
inline void loopmap_reset() { ++loopmapStubResets(); }
// The details box: a card "touched" (the demo), shown until closed
inline bool& loopmapStubDetails() { static bool b = false; return b; }
inline int& loopmapStubCardTaps() { static int n = 0; return n; }
inline bool loopmap_cardPoint(int, int& x, int& y) { x = 470; y = 510; return true; }
inline bool loopmap_tap(int, int) { ++loopmapStubCardTaps(); loopmapStubDetails() = true; return true; }
inline bool loopmap_detailsOpen() { return loopmapStubDetails(); }
inline void loopmap_closeDetails() { loopmapStubDetails() = false; }
}
