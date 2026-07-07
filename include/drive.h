#ifndef __DRIVE_H__
#define __DRIVE_H__

#include <queue>
#include <cstring>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <cstdio>
#include "hpil.h"
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"

#define BUF_SIZE    256
#define REC_SIZE    256
#define TRACKS      2
#define TAPE_SIZE   (TRACKS*REC_SIZE*BUF_SIZE)

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


class CTape {
protected:
    char _name[64];
public:
    CTape() {}

    virtual unsigned int tell(void) = 0;
    virtual void read(unsigned char *buf) = 0;
    virtual unsigned int readInt() = 0;
    virtual void write(unsigned char *buf) = 0;
    virtual void seek(unsigned int s) = 0;
    virtual void open(const char *name = "tape.bin") = 0;
    virtual void close() = 0;
};

// CTape - file on SD-card version
class CTapeSD : public CTape {
    FIL _tape;
    FRESULT _fr;
    bool _open = false;
public:
    CTapeSD() : CTape() {
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
                cdc0_printf("f_close error: %s (%d)\r\n", FRESULT_str(_fr), _fr);
                return;
            }
        }
        cdc0_printf("Opening tape file: [%s]\r\n", name);
        _fr = f_open(&_tape, name, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
        if (_fr != FR_OK) {
            cdc0_printf("f_open: %s (%d)\r\n", FRESULT_str(_fr), _fr);
            _name[0] = '\0';
            _open = false;
        } else {
            _open = true;
            strncpy(_name, name, sizeof(_name) - 1);
            _name[sizeof(_name) - 1] = '\0';
        }
    }
    void close() {
        cdc0_printf("Closing tape file: [%s]\r\n", _name);
        f_close(&_tape);
        _open = false;
        _name[0] = '\0';
    }
};

// Internal RAM version of tape for testing without SD-card
// Note - clears after each boot - so just for testing!!
class CTapeMem : public CTape {
    unsigned char   _tape[TAPE_SIZE];
    unsigned int    _tPos;
public:
    CTapeMem() : CTape() {
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
        cdc0_printf("Writing %d bytes to tape at %d\r\n", BUF_SIZE, tell());
        memcpy(_tape + _tPos, buf, BUF_SIZE);
        _tPos += BUF_SIZE;
    }
    void seek(unsigned int s) {
        _tPos = s;
    }
    void open(const char *name = "memory.bin") {
        cdc0_printf("Opening tape file: [%s]\r\n", name);
        _tPos = 0;
    }
    void close() {
        cdc0_printf("Closing tape file: [%s]\r\n", "memory.bin");
        _tPos = 0;
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
    CTapeFlash() : CTape(), _tPos(0), _loadedSector(-1), _dirty(false) {
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

    void open(const char *name = "FLASH_MEM") override {
        cdc0_printf("Opening flash tape [%s] @ offset 0x%06X\r\n", name, TAPE_FLASH_OFFSET);
        _tPos = 0;
    }

    void close() override {
        cdc0_printf("Closing flash tape, flushing sector %d\r\n", _loadedSector);
        flushSector();
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
    CTape           *tape;
    IL_DATA_t       buffer[TRACKS][BUF_SIZE];
    size_t          size;
public:
    CDrive(const char *name, CTape *_tape, IL_ADDR_t _sai=16, IL_ADDR_t _aau=2) : CDevice(name, _sai, _aau) {
        tape = _tape;
    }
    //IL_CMD_t hpil(IL_CMD_t cmd);
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
};

extern void sd_dir();

#endif//__DRIVE_H__
