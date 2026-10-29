#pragma once
// Host stand-in for the character table (view tests)
namespace hipi {
inline void chartable_drawAll() {}
inline bool chartable_tap(int, int) { return false; }
inline bool chartable_detailsOpen() { return false; }
inline int chartable_selected() { return -1; }
inline void chartable_step(int) {}
inline void chartable_closeDetails() {}
inline void chartable_reset() {}
}
