#ifndef __DRIVE_H__
#define __DRIVE_H__

#include <string>
#include <queue>
#include <cstring>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <cstdio>
#include "hpil.h"
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"

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

//#define MEDIA_NAME "HDRIVCHUU260701.DAT"
//#define MEDIA_NAME "HDRIVCHUU260708.DAT"
#define MEDIA_NAME "cass1.dat"

typedef struct media_t {
    const char* media;
    unsigned short int tracks;
    unsigned short int surfaces;
    unsigned short int blocks;
} Media_t;

// ─── Flash placement ───────────────────────────────────────────────────────────
// Pico 2 has 4 MB of flash. Place tape data at the very top.
// 128 KB / 4 KB = 32 sectors — must both be sector-aligned.
static_assert(TAPE_SIZE   % FLASH_SECTOR_SIZE == 0, "TAPE_SIZE not sector-aligned");
static_assert(BUF_SIZE    == FLASH_PAGE_SIZE,        "BUF_SIZE must be 256 (FLASH_PAGE_SIZE)");

#define FLASH_TOTAL_BYTES  (4u * 1024u * 1024u)
#define TAPE_FLASH_OFFSET  (FLASH_TOTAL_BYTES - TAPE_SIZE)   // 0x003E0000

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

#define SIZE_OFFS   24

class CTape {
protected:
    char _name[64];
    bool _open;
public:
    CTape(const char *name = MEDIA_NAME) {
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
        if( ok() )
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
    CTapeSD(const char *name = MEDIA_NAME) : CTape(name) {
        //open();
    }
    unsigned int tell(void) {
        return f_tell(&_tape);
    }
    void read(unsigned char *buf) {
        unsigned int n = 0;
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
        cdc0_printf("%s error: %s (%d)\n", str, FRESULT_str(_fr), _fr);
        tud_cdc_n_write_flush(0);
        tud_task();
    }
    void write(unsigned char *buf) {
        //printf("Writing %d bytes to tape at %d\n", BUF_SIZE, tell());
        unsigned int n;
        _fr = f_write(&_tape, buf, BUF_SIZE, &n);
        if (FR_OK != _fr)
            error("f_write");
    }
    void seek(unsigned int s) {
        _fr = f_lseek(&_tape, (FSIZE_t)s);
        if (_fr != FR_OK)
            error("f_lseek");
    }
    void open() { //const char *name = MEDIA_NAME) {
        cdc0_printf("Opening tape SD-file: [%s]\r\n", _name);
        tud_cdc_n_write_flush(0);
        tud_task();
        if( _open )
            close();
        _fr = f_open(&_tape, _name, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
        if (_fr != FR_OK) {
            error("f_open");
            //_name[0] = '\0';
            _open = false;
        } else {
            //strncpy(_name, name, sizeof(_name) - 1);
            //_name[sizeof(_name) - 1] = '\0';
            _open = true;
        }
    }
    void close() {
        cdc0_printf("Closing tape SD-file: [%s]\r\n", _name);
        tud_cdc_n_write_flush(0);
        tud_task();
        _fr = f_close(&_tape);
        if (_fr != FR_OK)
            error("f_close");
        //_name[0] = '\0';
        _open = false;
    }
};

// Internal RAM version of tape for testing without SD-card
// Note - clears after each boot - so just for testing!!
class CTapeMem : public CTape {
    unsigned char   _tape[TAPE_SIZE];
    unsigned int    _tPos;
public:
    CTapeMem(const char *name = MEDIA_NAME) : CTape(name) {
        open();
    }
    unsigned int tell(void) {
        return _tPos;
    }
    unsigned char *pos(void) {
        return _tape + tell();
    }
    void read(unsigned char *buf) {
        // Number of bytes to read ...
        int sz = (tell() + BUF_SIZE) >= TAPE_SIZE ? TAPE_SIZE - tell() : BUF_SIZE;
        // If less than BUF_SIZE then fill with 255 ...
        if( sz < BUF_SIZE )
            memset(buf+sz, 255, BUF_SIZE-sz);
        memcpy(buf, pos(), sz);
        wind(sz);
    }
    unsigned int readInt() {
        unsigned int n = *((unsigned int*)pos());
        wind(sizeof(unsigned int));
        return n;
    }
    void write(unsigned char *buf) {
        cdc0_printf("Writing %d bytes to tape at %d\r\n", BUF_SIZE, tell());
        memcpy(pos(), buf, BUF_SIZE);
        wind(BUF_SIZE);
    }
    void seek(unsigned int s) {
        _tPos = s;
    }
    void wind(unsigned int s) {
        seek(tell() + s);
    }
    void open(void) {
        cdc0_printf("Opening tape in RAM\r\n");
        seek(0);
        _open = true;
    }
    void close() {
        cdc0_printf("Closing tape in RAM\r\n");
        seek(0);
        _open = false;
    }
};

// Internal Flash version of tape for testing without SD-card
// Persistent storage - limited to 128 KB (32 sectors of 4 KB each)
// Note - not bit wear efficient - each write rewrites the entire sector!
class CTapeFlash : public CTape {
    uint32_t        _tPos;
    uint8_t         _sectorBuf[FLASH_SECTOR_SIZE];   // 4 KB RAM buffer
    int32_t         _loadedSector;                   // which sector is in buf (-1 = none)
    bool            _dirty;                          // buf differs from flash

    // ── helpers ───────────────────────────────────────────────────────────────

    int sectorOf(uint32_t tapePos) const {
        return (int)(tapePos / FLASH_SECTOR_SIZE);
    }

    // Read one sector from XIP-mapped flash into _sectorBuf (no erase needed)
    void loadSector(int sector) {
        if (_loadedSector == sector) return;
        flushSector();                               // write previous sector first
        uint32_t addr = XIP_BASE + TAPE_FLASH_OFFSET + (uint32_t)sector * FLASH_SECTOR_SIZE;
        memcpy(_sectorBuf, reinterpret_cast<const uint8_t*>(addr), FLASH_SECTOR_SIZE);
        _loadedSector = sector;
        _dirty = false;
    }

    // Erase + reprogram the buffered sector back to flash
    // NOTE: called with interrupts off; both flash_range_* run from ROM (safe)
    void flushSector() {
        if (_loadedSector < 0 || !_dirty) return;
        uint32_t off = TAPE_FLASH_OFFSET + (uint32_t)_loadedSector * FLASH_SECTOR_SIZE;
        cdc0_printf("Flush to flash at %u\r\n", off);
        uint32_t irq = save_and_disable_interrupts();
        flash_range_erase  (off, FLASH_SECTOR_SIZE);
        flash_range_program(off, _sectorBuf, FLASH_SECTOR_SIZE);
        restore_interrupts(irq);
        _dirty = false;
    }

public:
    CTapeFlash(const char *name = MEDIA_NAME) : CTape(name), _tPos(0), _loadedSector(-1), _dirty(false) {
        open();
    }

    ~CTapeFlash() { flushSector(); }

    // ── CTape interface ───────────────────────────────────────────────────────

    unsigned int tell(void) override { return _tPos; }

    void seek(unsigned int s) override { _tPos = s; }

    // Reads go straight through the XIP window — no sector buffer needed
    void read(unsigned char *buf) override {
        int sz = BUF_SIZE;
        if ((_tPos + BUF_SIZE) >= TAPE_SIZE)
            sz = (int)(TAPE_SIZE - _tPos);
        const uint8_t *src = reinterpret_cast<const uint8_t*>(
            XIP_BASE + TAPE_FLASH_OFFSET + _tPos);
        memcpy(buf, src, sz);
        while (sz < BUF_SIZE) buf[sz++] = 0xFF;
        _tPos += (uint32_t)sz;
    }

    unsigned int readInt() override {
        const uint8_t *src = reinterpret_cast<const uint8_t*>(
            XIP_BASE + TAPE_FLASH_OFFSET + _tPos);
        unsigned int n;
        memcpy(&n, src, sizeof(n));   // safe unaligned read
        _tPos += sizeof(unsigned int);
        return n;
    }

    // Writes go into the sector buffer; flash is only touched on sector change or close()
    void write(unsigned char *buf) override {
        cdc0_printf("Writing %d bytes to flash at %u\r\n", BUF_SIZE, _tPos);
        int sector = sectorOf(_tPos);
        loadSector(sector);                           // load if not already buffered
        uint32_t offsetInSector = _tPos % FLASH_SECTOR_SIZE;
        memcpy(_sectorBuf + offsetInSector, buf, BUF_SIZE);
        _dirty = true;
        _tPos += BUF_SIZE;
        flushSector();   // ← always flush immediately
    }

    void open(void) override {
        cdc0_printf("Opening flash tape [%s] @ offset 0x%06X\r\n", _name, TAPE_FLASH_OFFSET);
        _tPos = 0;
        _open = true;
    }

    void close() override {
        cdc0_printf("Closing flash tape, flushing sector %d\r\n", _loadedSector);
        flushSector();
        _tPos = 0;
        _open = false;
    }
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
    CDrive(const char *name, CTape *_tape, IL_ADDR_t _sai=16, IL_ADDR_t _aau=2) : CDevice(name, _sai, _aau) {
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
