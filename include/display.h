#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <cstdint>
#include <queue>
#include "hpil.h"

class CDisplay : public CDevice {
    std::queue<unsigned char> fifo;
    std::uint32_t totalPushed_ = 0;  // see totalPushed()'s own comment
public:
    CDisplay(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau=31) : CDevice(name, _sai, _aau, DISPLAY) {
    }
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void show(void);
    void clear(void);
    void idle(void);
    void ifc(void);
    // Exposes fifo's current size for pico_main.cpp's own periodic
    // diagnostic logging -- doListener() pushes into this queue whenever
    // HP-IL data arrives, but idle() only pops (and processes via
    // Screen::pr_char()) ONE byte per call, and only during genuinely
    // idle main-loop iterations (no HP-IL frame received that pass) --
    // if data arrives faster than that drains it, this queue has no
    // upper bound at all and could grow without limit, a completely
    // separate potential memory-growth source from the one already fixed
    // in Screen's own line buffer.
    std::size_t fifoSize() const { return fifo.size(); }
    // Running total of bytes ever pushed into fifo (never reset) --
    // compare against Screen::charsDrawn() to see whether drawing keeps
    // up with reception or silently falls behind/stops partway through
    // a long text stream.
    std::uint32_t totalPushed() const { return totalPushed_; }
};

extern CDisplay* videoDisplay;   // Same pattern as pilbox.h's `extern CPilBox* pilbox;`

#endif//__DISPLAY_H__
