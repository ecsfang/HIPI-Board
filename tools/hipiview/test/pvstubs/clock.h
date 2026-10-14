#pragma once
// Host stand-in for the clock / screen saver: the test says whether the
// real-time clock is there and set, and sees what was to be shown
namespace hipi {
enum class SaverShow { Clock, Rain, Goose };
inline bool& clockStubAvailable() { static bool b = false; return b; }
inline SaverShow& clockStubShow() { static SaverShow s = SaverShow::Rain; return s; }
inline void clock_drawAll() {}
inline bool clock_available() { return clockStubAvailable(); }
inline void clock_setShow(SaverShow s, bool) { clockStubShow() = s; }
}
