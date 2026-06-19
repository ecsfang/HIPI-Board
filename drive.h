#ifndef __DRIVE_H__
#define __DRIVE_H__

#include <queue>
#include "hpil.h"

class CDrive {
    IL_Status_e     status;
    IL_Mode_e       mode;
    IL_ADDR_t       addr;
    IL_ADDR_t       ddl;
    IL_ADDR_t       ddt;
    IL_ADDR_t       sst;
    IL_CMD_t        last;
    bool            sai;
    bool            end;
    char            *sdi;
    unsigned int    tmp;
    std::queue<unsigned char> fifo;
public:
    CDrive() {
        status = STAT_NONE;
        addr = 31;
        sai = false;
        sdi = NULL;
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
    IL_CMD_t next(IL_CMD_t cmd) { return cmd; }
};

#endif//__DRIVE_H__
