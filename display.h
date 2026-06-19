#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <queue>
#include "hpil.h"

class CDisplay {
    IL_Status_e     status;
    IL_ADDR_t       addr;
    bool            sai;
    char            *sdi;
    std::queue<unsigned char> fifo;
public:
    CDisplay() {
        status = STAT_NONE;
        addr = 31;
        sai = false;
        sdi = NULL;
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
};

#endif//__DISPLAY_H__
