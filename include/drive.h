#ifndef __DRIVE_H__
#define __DRIVE_H__

#include <queue>
#include "hpil.h"

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
    FILE *_tape;
public:
    CTape() {
        open();
    }
    unsigned int tell(void) { return ftell(_tape); }
    void read(unsigned char *buf) {
        int n = fread(buf, 1, BUF_SIZE, _tape);
        // If less than BUF_SIZE fill with 255 ...
        while( n < BUF_SIZE ) {
            buf[n++] = 255;
        }
    }
    unsigned int readInt() {
        unsigned int n = 0;
        if( fread(&n, sizeof(unsigned int), 1, _tape) == 1 ) {
            return n;
        }
        return 0;
    }
    void write(unsigned char *buf) {
        printf("Writing %d bytes to tape at %d\n", BUF_SIZE, tell());
        fwrite(buf, 1, BUF_SIZE, _tape);
    }
    void seek(unsigned int s) {
        fseek(_tape, s, SEEK_SET);
    }
    void open(const char *name = "tape.bin") {
        _tape = fopen(name, "r+b");
    }
    void close() {
        if( _tape )
            fclose(_tape);
        _tape = NULL;
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
public:
    CDrive(const char *name, IL_ADDR_t _sai, IL_ADDR_t _aau) : CDevice(name, _sai, _aau) {
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

#endif//__DRIVE_H__
