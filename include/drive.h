#ifndef __DRIVE_H__
#define __DRIVE_H__

#include "hpil.h"
#include "tape.h"   // CTape, CTapeSD, CTapeMem, CTapeFlash

typedef enum {
    WRITE_MODE,
    PARTIAL_MODE
} IL_Mode_e;

enum {
    DRV_IDLE            = 0,
    DRV_NO_TAPE_ERROR   = 20,
    DRV_NEW_TAPE_ERROR  = 23,
    DRV_SIZE_ERROR      = 28,
    DRV_BUSY            = 32
};

class CDrive : public CDevice {
    IL_Mode_e       mode;
    IL_ADDR_t       ddl;
    IL_ADDR_t       ddt;
    IL_ADDR_t       sst;
    IL_CMD_t        last;
    bool            end;
    unsigned int    m_tmp;
    unsigned int    pt;
    CTape           *tape;
    IL_DATA_t       buffer[TRACKS][BUF_SIZE];
    size_t          m_size;
public:
    CDrive(const char *name, CTape *_tape, IL_ADDR_t _sai=16, IL_ADDR_t _aau=2) : CDevice(name, _sai, _aau, DRIVE) {
        tape = _tape;
        mode = WRITE_MODE;
        last = ddl = ddt = sst = 0;
        m_size = 0;
        pt = m_tmp = 0;
        end = false;
        memset(buffer, 0, sizeof(buffer));
    }
    IL_CMD_t next(IL_CMD_t cmd);
    void clear(void);
    void ifc(void);
    void preProc(IL_CMD_t cmd);
    bool check();
    void readblock();
    void writeblock();
    void exchangeBuf() {
        IL_DATA_t tmpBuf[BUF_SIZE];
        memcpy(tmpBuf, buffer[0], BUF_SIZE);
        memcpy(buffer[0], buffer[1], BUF_SIZE);
        memcpy(buffer[1], tmpBuf, BUF_SIZE);
    }
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn);
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    IL_CMD_t doNextTalker(IL_CMD_t cmd);
    IL_CMD_t doNextListener(IL_CMD_t cmd);
    void close() {
        tape->close();
    }
    void show();
    size_t size(void) {
        return m_size;
    }
    void size(size_t sz) {
        m_size = sz;
    }
    void selectMedia(const char *media) {
        tape->close();

    }
};

extern void sd_dir();

#endif//__DRIVE_H__
