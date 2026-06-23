#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <queue>
#include "hpil.h"

class CDisplay : public CDevice {
    std::queue<unsigned char> fifo;
public:
    CDisplay(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau) : CDevice(name, _sai, _aau) {
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
    void clear(void);
};

#endif//__DISPLAY_H__
