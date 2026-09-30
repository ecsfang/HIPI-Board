#ifndef __DRIVE_H__
#define __DRIVE_H__

#include "hpil.h"
#include "sd_paths.h"
#include "tape.h"   // CTape, CTapeSD
#include "pico/time.h"
#include <cstdint>
#include <functional>
#include <string>

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

// Position of the OFF-STANDBY-ON switch (HP82161A owner's manual, p. 7):
//   Off     -- drive off; only the switch turns it on again
//   Standby -- the controller can power the drive down (Loop Power Down)
//              and it wakes up again on the next HP-IL activity
//   On      -- drive on; the controller can't power it down
enum class DrivePower : std::uint8_t { Off, Standby, On };

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
    // seeking, formatting, opening the tape file) -- NOT by plain HP-IL
    // traffic --
    // and held for kBusyHoldMs after the last access so even a single
    // short access is visible (see the Tape view's BUSY LED).
    static constexpr std::uint32_t kBusyHoldMs = 600;
    absolute_time_t busyUntil_ = nil_time;
    void markBusy() {                  // never shortens a longer busy period (REWIND)
        const absolute_time_t until = make_timeout_time_ms(kBusyHoldMs);
        if (absolute_time_diff_us(busyUntil_, until) > 0) busyUntil_ = until;
    }
    // Cassette "taken out" -- true while the file picker is open (the
    // drive's lid is open). The selected file stays selected, but the
    // drive reports no tape until it's put back (see check()).
    bool ejected_ = false;
    std::string mediaFile_;                            // see mediaFile()
    std::function<void(CDrive*)> onMediaChanged_;
    // Disabling (power switch / Devices menu) is deferred until the drive
    // is at a safe point -- see requestEnabled()/servicePendingDisable().
    // Pulling the drive out of the loop mid-transfer hung HP-IL.
    static constexpr std::uint32_t kQuietMs = 200;   // no message frames for this long = between messages
    absolute_time_t lastFrame_ = nil_time;
    bool disablePending_ = false;
    DrivePower powerMode_ = DrivePower::On;
    DrivePower lastOnMode_ = DrivePower::On;   // mode to return to after Off
    bool asleep_ = false;                      // Standby + powered down by LPD
    // Waking up from STANDBY takes a moment, during which the drive -- not
    // powered yet -- doesn't retransmit frames (DRVTST checks that the
    // frames right after LPD do NOT come back, and that the loop works
    // again shortly after: "LPD ERROR" / "LPU ERROR").
    static constexpr std::uint32_t kPowerUpMs = 150;
    bool poweringUp_ = false;
    bool lpdPending_ = false;                  // LPD seen, executed on the next RFC
    absolute_time_t powerUpUntil_ = nil_time;
    // REWIND button: the tape is "rewinding" (drive busy) until this time
    // Rewind time grows with how far into the tape we are: kRewindMinMs
    // just after the start, kRewindMaxMs at the very end (linear between)
    static constexpr std::uint32_t kRewindMinMs = 2000;
    static constexpr std::uint32_t kRewindMaxMs = 10000;
    absolute_time_t rewindUntil_ = nil_time;
    // What the drive does when it (re)gains power (switched on, or woken
    // after Loop Power Down): as Device Clear + Interface Clear (manual:
    // startup conditions / LPD).
    // The manual also says the address becomes undefined, but the HP-41
    // doesn't re-address the loop in these cases -- DRVTST sends TAD <old
    // address> straight away and got NO RESPONSE. So the address is kept.
    // If the controller DID re-address the loop while the drive was off,
    // the old address is dropped then instead: see CDevice::offLoopFrame().
    void powerUpReset() {
        setIdle();
        ifc();
        clear();
        // (Re)opening the file reports "New Tape" (status 23): the HP82161A
        // service manual's DRVTST expects exactly that after a power-up.
    }
public:
    ViewKind viewKind() const override { return ViewKind::Tape; }
    IL_CMD_t hpil(IL_CMD_t cmd) override;
    DrivePower powerMode() const { return powerMode_; }
    DrivePower lastOnMode() const { return lastOnMode_; }
    // Switch position. Off is deferred until the drive is idle (see
    // requestEnabled()); Standby/On take effect at once.
    void setPowerMode(DrivePower m) {
        if (m == powerMode_) return;
        const bool wasOff = !enabled();
        if (m == DrivePower::Off) {
            requestEnabled(false);
        } else {
            lastOnMode_ = m;
            requestEnabled(true);
            if (wasOff || asleep_) {       // really powering up
                asleep_ = false;
                powerUpReset();
            }
        }
        powerMode_ = m;
        LOGF("\r\n * %s: switch %s", name(),
             m == DrivePower::Off ? "OFF" : m == DrivePower::Standby ? "STANDBY" : "ON");
    }
    // Boot: restore the saved switch position without any power-up
    // actions (the enabled state is already applied from the config)
    void initPowerMode(DrivePower m, DrivePower lastOn) {
        powerMode_ = m;
        lastOnMode_ = lastOn;
        asleep_ = false;
    }
    // REWIND button (owner's manual p. 8): rewinds the tape to its start.
    // Doesn't operate while the BUSY light is on (busyLight(), which
    // includes being addressed), powered down, without a cassette, or
    // when the tape is already at its start. The
    // drive is busy for kRewindMinMs..kRewindMaxMs, in proportion to the
    // tape position -- BUSY light on, reels spinning
    // backwards, HP-IL access answered with the Busy status (32) -- as a
    // real rewind takes a few seconds. Returns false if ignored.
    bool manualRewind() {
        if (!poweredUp() || busyLight() || ejected_ ||   // manual: not while BUSY is lit
            tape->media() == nullptr || *tape->media() == 0) {
            return false;
        }
        // Already at the start (or at the leader after an earlier rewind):
        // nothing to rewind. A closed file counts as at the start -- it
        // is opened at position 0.
        if (!tape->ok() || tape->tell() == 0) {
            LOGF("\r\n * %s: REWIND ignored -- tape already at the start", name());
            return false;
        }
        // Position as a fraction of the tape (size() is in 256-byte records)
        const std::uint64_t pos = tape->tell();
        const std::uint64_t total = static_cast<std::uint64_t>(size() > 0 ? size() : 512) * BUF_SIZE;
        const std::uint64_t part = pos < total ? pos : total;
        const std::uint32_t ms = kRewindMinMs +
            static_cast<std::uint32_t>((kRewindMaxMs - kRewindMinMs) * part / total);
        tape->seek(0);
        rewindUntil_ = make_timeout_time_ms(ms);
        busyUntil_ = rewindUntil_;
        LOGF("\r\n * %s: REWIND from %lu/%lu (%lu ms)", name(),
             static_cast<unsigned long>(pos), static_cast<unsigned long>(total),
             static_cast<unsigned long>(ms));
        return true;
    }
    bool isRewinding() const { return !time_reached(rewindUntil_); }
    // BUSY light, as in the owner's manual (p. 8): on while the drive is
    // executing an operation (isBusy() -- file access, seek, rewind) OR is
    // addressed as talker or listener. The spinning reels still follow
    // isBusy() only, i.e. real tape movement.
    bool busyLight() { return isBusy() || isTalker() || isListener(); }
    // POWER light: on while the drive is in its operating power state
    bool poweredUp() const { return enabled() && !asleep_ && !poweringUp_; }
    // Finishes a pending power-up once kPowerUpMs has passed. Called for
    // every frame (hpil()) and from the main loop (boardui_poll()), so it
    // completes even if no further frame arrives.
    void servicePowerUp() {
        if (poweringUp_ && time_reached(powerUpUntil_)) {
            poweringUp_ = false;
            powerUpReset();
            LOGF("\r\n * %s: powered up", name());
        }
    }
    // True while the drive is (or very recently was) accessing the file
    bool isBusy() const { return !time_reached(busyUntil_); }
    // Each drive has its own cassette: an SD-card LIF (.dat) file, "" = no
    // media. Several drives can be on the loop, each with its own file.
    CDrive(const char *name, const std::string& mediaFile, IL_ADDR_t _sai=16, IL_ADDR_t _aau=2)
        : CDrive(name, new CTapeSD(lifPath(mediaFile).c_str()), _sai, _aau) {
        mediaFile_ = mediaFile;
    }
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
        LOGF("\r\n * %s: cassette %s", name(), ejected_ ? "removed" : "inserted");
    }
    bool ejected() const { return ejected_; }

    // The drive's cassette file (see the constructor). setMediaFile()
    // "inserts" another one: the old file is closed, the new one is opened
    // on the next access (reported as a new tape). The callback is told,
    // e.g. to save it in CONFIG.TXT and update the Tape view.
    const std::string& mediaFile() const { return mediaFile_; }
    // The file on the SD card: lif/<name> ("" stays "" = no media)
    static std::string lifPath(const std::string& file) {
        return file.empty() ? file : std::string(HIPI_DIR_LIF "/") + file;
    }
    void setMediaFile(const std::string& file) {
        tape->select(lifPath(file).c_str());
        mediaFile_ = file;
        LOGF("\r\n * %s: media %s", name(), file.empty() ? "(none)" : file.c_str());
        if (onMediaChanged_) onMediaChanged_(this);
    }
    void setMediaChangedCallback(std::function<void(CDrive*)> cb) { onMediaChanged_ = std::move(cb); }

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
};


#endif//__DRIVE_H__
