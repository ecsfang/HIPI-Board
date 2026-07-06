#ifndef __ILLEDS_H__
#define __ILLEDS_H__

#include <queue>
#include "hpil.h"

class CLed : public CDevice {
public:
    CLed(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau=31) : CDevice(name, _sai, _aau) {
    }
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void clear(void);
    void ifc(void);
};

#endif//__ILLEDS_H__
