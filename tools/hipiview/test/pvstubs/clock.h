#pragma once
namespace hipi {
enum class SaverShow { Clock, Rain, Goose };
inline void clock_drawAll() {}
inline bool clock_available() { return false; }
inline void clock_setShow(SaverShow, bool) {}
}
