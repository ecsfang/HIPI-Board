#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <queue>
#include "hpil.h"

class CDisplay : public CDevice {
    std::queue<unsigned char> fifo;
public:
    CDisplay(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau=31) : CDevice(name, _sai, _aau) {
    }
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void show(void);
    void clear(void);
    void idle(void);
    void ifc(void);
};

#endif//__DISPLAY_H__
