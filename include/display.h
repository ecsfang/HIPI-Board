#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <queue>
#include "hpil.h"

class CDisplay : public CDevice {
    std::queue<unsigned char> fifo;
    bool bPrt;
public:
    CDisplay(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau=31) : CDevice(name, _sai, _aau) {
        bPrt = false;
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
    void clear(void);
    void idle(void);
};

#endif//__DISPLAY_H__
