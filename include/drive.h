#ifndef __DRIVE_H__
#define __DRIVE_H__

#include "hpil.h"
#include "tape.h"   // CTape, CTapeSD, CTapeMem, CTapeFlash
#include "pico/time.h"
#include <cstdint>

extern void disableSD();
extern void enableSD();
extern bool sdCardOK();
extern void sd_dir();

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
    // BUSY indication: set by real file handling (block read/write,
    // formatting, opening the tape file) -- NOT by plain HP-IL traffic --
    // and held for kBusyHoldMs after the last access so even a single
    // short access is visible (see the Tape view's BUSY LED).
    static constexpr std::uint32_t kBusyHoldMs = 300;
    absolute_time_t busyUntil_ = nil_time;
    void markBusy() { busyUntil_ = make_timeout_time_ms(kBusyHoldMs); }
    // Cassette "taken out" -- true while the file picker is open (the
    // drive's lid is open). The selected file stays selected, but the
    // drive reports no tape until it's put back (see check()).
    bool ejected_ = false;
    // Disabling (power switch / Devices menu) is deferred until the drive
    // is at a safe point -- see requestEnabled()/servicePendingDisable().
    // Pulling the drive out of the loop mid-transfer hung HP-IL.
    static constexpr std::uint32_t kQuietMs = 200;   // no message frames for this long = between messages
    absolute_time_t lastFrame_ = nil_time;
    bool disablePending_ = false;
public:
    // True while the drive is (or very recently was) accessing the file
    bool isBusy() const { return !time_reached(busyUntil_); }
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
    // Takes the cassette out (true) or puts it back (false). While out,
    // check() reports DRV_NO_TAPE_ERROR exactly as with no file selected.
    // Putting it back reopens the (possibly newly selected) file on the
    // next access, reported like a newly inserted tape.
    void setEjected(bool ejected) {
        if (ejected == ejected_) return;
        ejected_ = ejected;
        if (ejected_ && tape->ok()) tape->close();   // flush and release the file
        LOGF("\r\n * Drive: cassette %s", ejected_ ? "removed" : "inserted");
    }
    bool ejected() const { return ejected_; }

    // Enable at once; disable only once it's safe (servicePendingDisable()).
    // Returns the new "wanted" state -- what to show and save.
    bool requestEnabled(bool on) {
        if (on) {
            disablePending_ = false;
            setEnabled(true);
        } else if (enabled()) {
            disablePending_ = true;
            LOGF("\r\n * %s: will switch off when idle", name());
        }
        return on;
    }
    bool disablePending() const { return disablePending_; }
    // Enabled and not about to be switched off
    bool wantedEnabled() const { return enabled() && !disablePending_; }
    // Call from the main loop (never from inside frame processing).
    // Switches off a pending disable once the drive is at a safe point:
    // no file access in progress (not busy) and no HP-IL message frame
    // (IDY polling doesn't count, see preProc()) for kQuietMs, i.e.
    // between messages, not inside one. Being left addressed as talker or
    // listener after the last message is fine -- the HP-41 routinely does
    // that, and requiring otherwise meant the drive never switched off.
    // Returns true when it actually switched off.
    bool servicePendingDisable() {
        if (!disablePending_) return false;
        const bool quiet = absolute_time_diff_us(lastFrame_, get_absolute_time())
                           >= static_cast<std::int64_t>(kQuietMs) * 1000;
        if (isBusy() || !quiet) return false;
        if (tape->ok()) tape->close();     // flush and release the file
        setEnabled(false);
        disablePending_ = false;
        LOGF("\r\n * %s switched off", name());
        return true;
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


#endif//__DRIVE_H__
