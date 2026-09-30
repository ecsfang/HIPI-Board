#ifndef __TAPE_H__
#define __TAPE_H__

// CTape (the cassette interface) and CTapeSD, its implementation on the
// SD card via FatFs -- one LIF image file per cassette (lif/<name>.dat).
// Split out of drive.h so the HP-IL device logic (CDrive) and the storage
// can be read/changed independently.

#include <string>
#include <cstring>
#include <cstdio>
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"
#include "usb_serial.h"  // LOGF

#define MEDIUM_CASS     0
#define MEDIUM_DISK     1
#define MEDIUM_HDRIVE1  2
#define MEDIUM_HDRIVE2  3
#define MEDIUM_HDRIVE4  4
#define MEDIUM_HDRIVE8  5
#define MEDIUM_HDRIVE16 6

#define BUF_SIZE    256
#define REC_SIZE    256
#define TRACKS      2
#define SURFACES    2
#define TAPE_SIZE   (TRACKS*REC_SIZE*BUF_SIZE)


#define SIZE_OFFS   24

typedef struct media_t {
    const char* media;
    unsigned short int tracks;
    unsigned short int surfaces;
    unsigned short int blocks;
} Media_t;

class CTape {
protected:
    char _name[64];
    bool _open;
public:
    CTape(const char *name = "") {
        _open = false;
        select(name);
    }
    virtual unsigned int tell(void) = 0;
    virtual void read(unsigned char *buf) = 0;
    virtual unsigned int readInt() = 0;
    virtual void write(unsigned char *buf) = 0;
    virtual void seek(unsigned int s) = 0;
    virtual void open(void) = 0;
    virtual void close(void) = 0;
    bool ok(void) {
        return _open;
    }
    void select(const char *name) {
        if( ok() ) // Close any previously open tape
            close();
        strcpy(_name, name);
    }
    void select(const std::string& name) {
        select( name.c_str());
    }
    unsigned int mediaSize() {
        return tracks()*surfaces()*blocks();
    }
    const char *media(void) {
        return _name;
    }
    unsigned int readInt(int offs) {
        seek(offs);
        return readInt();
    }
    unsigned int tracks() {
        return readInt(SIZE_OFFS);
    }
    unsigned int surfaces() {
        return readInt(SIZE_OFFS+4);
    }
    unsigned int blocks() {
        return readInt(SIZE_OFFS+8);
    }
};

// CTape - file on SD-card version
class CTapeSD : public CTape {
    FIL _tape;
    FRESULT _fr;
public:
    CTapeSD(const char *name = "") : CTape(name) {
    }
    unsigned int tell(void) {
        return _open ? f_tell(&_tape) : 0;
    }
    void read(unsigned char *buf) {
        unsigned int n = 0;
        if( !_open ) {
            error("read: tape not open");
            memset(buf, 255, BUF_SIZE);
            return;
        }
        _fr = f_read(&_tape, buf, BUF_SIZE, &n);
        if (FR_OK != _fr) {
            error( "f_read" );
            n = 0;
        }
        // If less than BUF_SIZE fill with 255 ...
        while( n < BUF_SIZE ) {
            buf[n++] = 255;
        }
    }
    unsigned int readByte() {
        unsigned int n;
        unsigned char b;
        if( !_open ) {
            error("readByte: tape not open");
            return 0;
        }
        _fr = f_read(&_tape, &b, 1, &n);
        if (FR_OK != _fr) {
            error( "f_read" );
            return 0;
        }
        return b;
    }
    unsigned int readInt() {
        unsigned int w = 0;
        for(int i=0; i<4; i++) {
            w = (w<<8) | readByte();
        }
        return w;
    }
    void error(const char *str) {
        LOGF("%s error: %s (%d)\n", str, FRESULT_str(_fr), _fr);
        tud_cdc_n_write_flush(0);
        tud_task();
    }
    void write(unsigned char *buf) {
        //printf("Writing %d bytes to tape at %d\n", BUF_SIZE, tell());
        unsigned int n;
        if( !_open ) {
            error("write: tape not open");
            return;
        }
        _fr = f_write(&_tape, buf, BUF_SIZE, &n);
        if (FR_OK != _fr)
            error("f_write");
    }
    void seek(unsigned int s) {
        if( !_open ) {
            error("seek: tape not open");
            return;
        }
        _fr = f_lseek(&_tape, (FSIZE_t)s);
        if (_fr != FR_OK)
            error("f_lseek");
    }
    void open() {
        LOGF("Opening tape SD-file: [%s]\r\n", _name);
        tud_cdc_n_write_flush(0);
        tud_task();
        if( _open )
            close();
        _fr = f_open(&_tape, _name, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
        if (_fr != FR_OK) {
            error("f_open");
            _open = false;
        } else {
            _open = true;
        }
    }
    void close() {
        LOGF("Closing tape SD-file: [%s]\r\n", _name);
        tud_cdc_n_write_flush(0);
        tud_task();
        if( !_open ) {
            LOGF("Nothing to close for: [%s]\r\n", _name);
            return;
        }
        _fr = f_close(&_tape);
        LOGF("Closing file ...\r\n");
        if (_fr != FR_OK)
            error("f_close");
        _open = false;
        LOGF("Done closing\r\n");
    }
};

#endif//__TAPE_H__
