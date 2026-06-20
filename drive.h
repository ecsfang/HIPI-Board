#ifndef __DRIVE_H__
#define __DRIVE_H__

#include <queue>
#include "hpil.h"

class CTape {
public:
    CTape() {}
    unsigned int tell(void) { return 1; }
    void write(unsigned char *buf) {}
    void seek(unsigned int s) {}
    void close() {}
};

class CDrive : public CDevice {
    IL_Mode_e       mode;
    IL_ADDR_t       ddl;
    IL_ADDR_t       ddt;
    IL_ADDR_t       sst;
    IL_CMD_t        last;
    bool            end;
    unsigned int    tmp;
    std::queue<unsigned char> fifo;
    unsigned int pt;
    CTape           tape;
    unsigned char   buf0[256];
    unsigned char   buf1[256];
    size_t          size;
public:
    CDrive(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau) : CDevice(name, _sai, _aau) {
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
    IL_CMD_t next(IL_CMD_t cmd);
    void clear(void);
    bool check();
    void readblock();
    void writeblock();
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn);
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
};

#endif//__DRIVE_H__
