#ifndef __DRIVE_H__
#define __DRIVE_H__

#include <queue>
#include <cstring>
#include "hpil.h"
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"

#define BUF_SIZE    256
#define REC_SIZE    256
#define TRACKS      2
#define TAPE_SIZE   (TRACKS*REC_SIZE*BUF_SIZE)

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

class CTape {
    FIL _tape;
    FRESULT _fr;
    bool _open = false;
    char _name[64];
public:
    CTape() {
        open();
    }
    unsigned int tell(void) {
        return f_tell(&_tape);
    }
    void read(unsigned char *buf) {
        unsigned int n = 0;
        _fr = f_read(&_tape, buf, BUF_SIZE, &n);
        if (FR_OK != _fr) {
            EMSG_PRINTF("f_read error: %s (%d)\n", FRESULT_str(_fr), _fr);
            n = 0;
        }
        // If less than BUF_SIZE fill with 255 ...
        while( n < BUF_SIZE ) {
            buf[n++] = 255;
        }
    }
    unsigned int readInt() {
        unsigned int n = 0;
        _fr = f_read(&_tape, &n, sizeof(unsigned int), &n);
        if (FR_OK != _fr) {
            EMSG_PRINTF("f_read error: %s (%d)\n", FRESULT_str(_fr), _fr);
            return 0;
        }
        return n;
    }
    void write(unsigned char *buf) {
        //printf("Writing %d bytes to tape at %d\n", BUF_SIZE, tell());
        unsigned int n;
        _fr = f_write(&_tape, buf, BUF_SIZE, &n);
        if (FR_OK != _fr) {
            EMSG_PRINTF("f_write error: %s (%d)\n", FRESULT_str(_fr), _fr);
        }
    }
    void seek(unsigned int s) {
        f_lseek(&_tape, s);
    }
    void open(const char *name = "tape.bin") {
        if (_open) {
            _fr = f_close(&_tape);
            if (_fr != FR_OK) {
                printf("f_close error: %s (%d)\n", FRESULT_str(_fr), _fr);
                return;
            }
        }
        printf("Opening tape file: [%s]\n", name);
        _fr = f_open(&_tape, name, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
        if (_fr != FR_OK) {
            printf("f_open: %s (%d)\n", FRESULT_str(_fr), _fr);
            _name[0] = '\0';
            _open = false;
        } else {
            _open = true;
            strncpy(_name, name, sizeof(_name) - 1);
            _name[sizeof(_name) - 1] = '\0';
        }
    }
    void close() {
        printf("Closing tape file: [%s]\n", _name);
        f_close(&_tape);
        _open = false;
        _name[0] = '\0';
    }
};

// Internal RAM version of tape for testing without SD-card
class CTapeMem {
    unsigned char   _tape[TAPE_SIZE];
    unsigned int    _tPos;
public:
    CTapeMem() {
        open();
    }
    unsigned int tell(void) {
        return _tPos;
    }
    void read(unsigned char *buf) {
        int sz = BUF_SIZE;
        if( (_tPos + BUF_SIZE) >= TAPE_SIZE )
            sz = TAPE_SIZE - _tPos;
        memcpy(buf, _tape + _tPos, sz);
        // If less than BUF_SIZE fill with 255 ...
        while( sz < BUF_SIZE ) {
            buf[sz++] = 255;
        }
        _tPos += sz;
    }
    unsigned int readInt() {
        unsigned int n = 0;
        n = *((unsigned int*)(_tape+_tPos));
        _tPos += sizeof(unsigned int);
        return n;
    }
    void write(unsigned char *buf) {
        printf("Writing %d bytes to tape at %d\n", BUF_SIZE, tell());
        memcpy(_tape + _tPos, buf, BUF_SIZE);
        _tPos += BUF_SIZE;
    }
    void seek(unsigned int s) {
        _tPos = s;
    }
    void open(const char *name = "memory.bin") {
        printf("Opening tape file: [%s]\n", name);
        _tPos = 0;
    }
    void close() {
        printf("Closing tape file: [%s]\n", "memory.bin");
        _tPos = 0;
    }
};

class CDrive : public CDevice {
    IL_Mode_e       mode;
    IL_ADDR_t       ddl;
    IL_ADDR_t       ddt;
    IL_ADDR_t       sst;
    IL_CMD_t        last;
    bool            end;
    unsigned int    tmp;
    std::queue<IL_DATA_t> fifo;
    unsigned int    pt;
    CTape           tape;
    IL_DATA_t       buf0[BUF_SIZE];
    IL_DATA_t       buf1[BUF_SIZE];
    size_t          size;
    bool bPrt;
public:
    CDrive(const char *name, IL_ADDR_t _sai=16, IL_ADDR_t _aau=2) : CDevice(name, _sai, _aau) {
        bPrt = false;
    }
    IL_CMD_t hpil(IL_CMD_t cmd);
    IL_CMD_t next(IL_CMD_t cmd);
    void clear(void);
    bool check();
    void readblock();
    void writeblock();
    void exchangeBuf() {
        for(int i=0; i<BUF_SIZE; i++) {
            IL_DATA_t u = buf0[i];
            buf0[i] = buf1[i];
            buf1[i] = u;
        }
    }
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn);
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    IL_CMD_t doNextTalker(IL_CMD_t cmd);
    IL_CMD_t doNextListener(IL_CMD_t cmd);
    void close() {
        tape.close();
    }
};

extern void sd_dir();

#endif//__DRIVE_H__
