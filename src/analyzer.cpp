#define MODULE "ANLZ"
#include "analyzer.h"
#include "sd_paths.h"
#include "usb_msc.h"
#include "usb_serial.h"
#include "boardui.h"
#include "plotterview.h"
#include "backlight.h"
#include "ff.h"
#include "pico/time.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <algorithm>
#include <vector>

extern std::vector<CDevice*> devices;   // hipi.cpp -- loop order

namespace hipi {

namespace {

// ── Capture ring (filled from hipi_loop()) ──────────────────────────────
struct Capture {
    std::uint32_t tUs;      // time_us_32() at arrival
    std::uint16_t in;       // frame as it arrived on IN
    std::uint16_t out;      // frame as it left (IL_NO_FRAME = absorbed)
};
constexpr std::uint32_t kRingSize = 4096;    // 32 kB
Capture ring_[kRingSize];
volatile std::uint32_t head_ = 0;            // entries captured so far
std::uint32_t frames_ = 0;                   // frames seen (for the title)

// Idle polling (IDY frames passed on unchanged) can arrive by the thousand
// and would push the real traffic out of the ring -- so an unbroken run of
// the same IDY frame is stored as ONE entry: out = kRunFlag | count.
// The run being counted is kept here until a different frame arrives (or
// it has been quiet for a while, see analyzer_poll()).
constexpr std::uint16_t kRunFlag = 0x8000;   // frames are 11 bits -- never set
constexpr std::uint16_t kRunMax  = 0x7FFE;   // (0xFFFF is IL_NO_FRAME)
std::uint16_t runIn_ = 0;
std::uint16_t runCount_ = 0;
std::uint32_t runT_ = 0;                     // first frame of the run
std::uint32_t runLastT_ = 0;                 // latest frame of the run

bool isRun(const Capture& c) {
    return c.out != IL_NO_FRAME && (c.out & kRunFlag) != 0;
}

void push(std::uint32_t tUs, std::uint16_t in, std::uint16_t out) {
    Capture& c = ring_[head_ % kRingSize];
    c.tUs = tUs;
    c.in = in;
    c.out = out;
    head_ = head_ + 1;
}

void flushRun() {
    if (runCount_ == 0) return;
    push(runT_, runIn_, static_cast<std::uint16_t>(kRunFlag | runCount_));
    runCount_ = 0;
}
std::uint32_t analysed_ = 0;                 // frames analysed so far
std::uint32_t lost_ = 0;                     // overrun, not analysed

// ── Output lines ────────────────────────────────────────────────────────
constexpr std::uint16_t kWhite  = 0xFFFF;
constexpr std::uint16_t kYellow = 0xFFE0;
constexpr std::uint16_t kRed    = 0xF800;
constexpr std::uint16_t kGreen  = 0x07E0;
constexpr std::uint16_t kCyan   = 0x07FF;
constexpr std::uint16_t kGrey   = 0x8410;

struct Line {
    std::uint64_t t;        // time since start-up, µs
    std::uint16_t color;
    std::string text;
};
constexpr std::size_t kMaxLines = 600;
std::deque<Line> lines_;

// ── Settings / view state ───────────────────────────────────────────────
DisplayDriver* d_ = nullptr;
AnalyzerMode mode_ = AnalyzerMode::Overview;
bool relative_ = false;
bool showIdle_ = false;
bool paused_ = false;
int scroll_ = 0;                             // lines back from the newest
bool dirty_ = true;
bool forceDraw_ = false;                     // draw once even while paused
absolute_time_t nextDraw_ = nil_time;
constexpr std::uint32_t kDrawIntervalMs = 150;

// ── Log file ────────────────────────────────────────────────────────────
FIL logFile_;
bool logging_ = false;
std::string logName_;
int logUnsynced_ = 0;

// ── Analysis state ──────────────────────────────────────────────────────
constexpr std::uint64_t kSlowUs = 500000;    // "slow" gap inside a transaction

enum class Tx { None, CtrlSend, DevSend };
struct State {
    int talker = -1;                         // address, -1 = none
    std::uint32_t listeners = 0;             // bit per address
    Tx tx = Tx::None;
    std::uint64_t txStart = 0;
    std::uint64_t lastT = 0;
    int txBytes = 0;
    std::string txText;                      // first printable bytes
    bool txBinary = false;
    bool txCrLf = false;
    bool txTrunc = false;                    // more text than txText holds
    IL_CMD_t txReq = 0;                      // SDA/SST/SAI/SDI
    bool txAnswered = false;
    int pendingDdl = -1;                     // device-dependent listener cmd
    int pendingDdt = -1;                     // device-dependent talker cmd
    std::vector<int> ddlArgs;
    IL_CMD_t lastCmd = 0;                    // last CMD/RDY frame
    std::uint64_t lastCmdT = 0;
    std::uint32_t idle = 0;                  // IDY frames not shown
    std::uint8_t aid[32] = {};               // accessory IDs seen, 0 = unknown
    // The loop's controller. HP-IL frames don't say who sent them, so it
    // can't be identified (HP-41, HP-71, a PC...) -- just "CTRL", until
    // control is passed on with TAKE CONTROL (TCT) to a known talker.
    std::string ctrl = "CTRL";
    // Addresses taken by devices on the PC side of PILBOX (a PC emulator
    // such as pyILPER): the ones inside HIPI's range at auto-addressing
    // that aren't HIPI's own devices
    std::uint32_t pcAddrs = 0;
    bool haveAid[32] = {};
};
State st_;

// ── Names ───────────────────────────────────────────────────────────────
CDevice* hipiDeviceAt(int addr) {
    for (CDevice* d : devices) {
        if (d->enabled() && static_cast<int>(d->addr()) == addr) return d;
    }
    return nullptr;
}

const char* aidClass(std::uint8_t aid) {
    switch (aid >> 4) {
        case 0x1: return "mass storage";
        case 0x2: return "printer";
        case 0x3: return "display";
        case 0x4: return "interface";
        case 0x5: return "instrument";
        case 0x6: return "graphics";
        default:  return "device";
    }
}

std::string nameOf(int addr) {
    if (addr < 0) return "?";
    if (CDevice* d = hipiDeviceAt(addr)) {
        char b[32];
        std::snprintf(b, sizeof(b), "%s(%d)", d->name(), addr);
        return b;
    }
    char b[48];
    // Devices on the PC behind PILBOX (seen at the last auto-addressing)
    const char* pc = (addr < 32 && (st_.pcAddrs & (1u << addr))) ? "PC " : "";
    if (addr < 32 && st_.haveAid[addr])
        std::snprintf(b, sizeof(b), "%saddr %d (%s)", pc, addr, aidClass(st_.aid[addr]));
    else
        std::snprintf(b, sizeof(b), "%saddr %d", pc, addr);
    return b;
}

std::string listenerNames(std::uint32_t mask) {
    if (mask == 0) return "(no listener)";
    std::string s;
    for (int a = 0; a < 31; ++a) {
        if (mask & (1u << a)) {
            if (!s.empty()) s += ", ";
            s += nameOf(a);
        }
    }
    return s;
}

bool talkerIsDrive() {
    CDevice* d = hipiDeviceAt(st_.talker);
    return d && d->type() == DRIVE;
}
bool listenerIsDrive() {
    for (int a = 0; a < 31; ++a) {
        if (st_.listeners & (1u << a)) {
            CDevice* d = hipiDeviceAt(a);
            if (d && d->type() == DRIVE) return true;
        }
    }
    return false;
}

// HP82161A device-dependent commands (owner's manual pp. 11-13)
const char* driveDdlName(int n) {
    static const char* names[] = { "WRITE BUFFER 0", "WRITE BUFFER 1", "WRITE", "SET BYTE POINTER",
                                   "SEEK", "FORMAT", "PARTIAL WRITE", "REWIND", "CLOSE RECORD",
                                   "TRANSFER BUFFER", "EXCHANGE BUFFERS" };
    return n >= 0 && n <= 10 ? names[n] : "IGNORE DATA";
}
const char* driveDdtName(int n) {
    static const char* names[] = { "SEND BUFFER 0", "SEND BUFFER 1", "READ", "SEND POSITION",
                                   "EXCHANGE BUFFERS" };
    return n >= 0 && n <= 4 ? names[n] : "NO DATA";
}
const char* driveStatus(int s) {
    if (s < 16) return "OK";
    if (s >= 32) return "BUSY";
    switch (s) {
        case 17: return "END OF TAPE";
        case 18: return "STALL";
        case 19: return "END OF TAPE + STALL";
        case 20: return "NO TAPE";
        case 21: case 22: return "DEVICE ERROR";
        case 23: return "NEW TAPE";
        case 24: return "TIME OUT";
        case 25: return "RECORD NUMBER ERROR";
        case 26: return "CHECKSUM ERROR";
        case 28: return "SIZE ERROR";
        default: return "?";
    }
}

std::string mnem(IL_CMD_t f) {
    char b[32];
    ilMnemonic(f, b);
    std::string s(b);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// ── Emitting ────────────────────────────────────────────────────────────
std::string timeText(std::uint64_t t, std::uint64_t prevT) {
    char b[24];
    if (relative_) {
        const std::uint64_t d = t >= prevT ? t - prevT : 0;
        std::snprintf(b, sizeof(b), "+%5lu.%03lu", static_cast<unsigned long>(d / 1000000),
                      static_cast<unsigned long>((d / 1000) % 1000));
    } else {
        const std::uint64_t ms = t / 1000;
        std::snprintf(b, sizeof(b), "%02lu:%02lu:%02lu.%03lu",
                      static_cast<unsigned long>(ms / 3600000), static_cast<unsigned long>((ms / 60000) % 60),
                      static_cast<unsigned long>((ms / 1000) % 60), static_cast<unsigned long>(ms % 1000));
    }
    return b;
}

void logLine(const Line& l, std::uint64_t prevT) {
    if (!logging_) return;
    const std::string s = timeText(l.t, prevT) + "  " + l.text + "\r\n";
    UINT bw = 0;
    f_write(&logFile_, s.data(), static_cast<UINT>(s.size()), &bw);
    if (++logUnsynced_ >= 20) {
        f_sync(&logFile_);
        logUnsynced_ = 0;
    }
}

void emit(std::uint64_t t, std::uint16_t color, const std::string& text) {
    const std::uint64_t prevT = lines_.empty() ? t : lines_.back().t;
    lines_.push_back(Line{ t, color, text });
    while (lines_.size() > kMaxLines) lines_.pop_front();
    logLine(lines_.back(), prevT);
    if (scroll_ > 0) ++scroll_;                // keep the view where it is
    dirty_ = true;
}

void emitIdle(std::uint64_t t) {
    if (showIdle_ && st_.idle > 0) {
        char b[48];
        std::snprintf(b, sizeof(b), "idle polling (IDY x %lu)", static_cast<unsigned long>(st_.idle));
        emit(t, kGrey, b);
    }
    st_.idle = 0;
}

std::string durText(std::uint64_t us) {
    char b[24];
    if (us >= 1000000) std::snprintf(b, sizeof(b), "%lu.%02lu s", static_cast<unsigned long>(us / 1000000),
                                     static_cast<unsigned long>((us / 10000) % 100));
    else std::snprintf(b, sizeof(b), "%lu.%lu ms", static_cast<unsigned long>(us / 1000),
                       static_cast<unsigned long>((us / 100) % 10));
    return b;
}

// Adds a byte to the transfer being summarised
void addByte(std::uint8_t b) {
    ++st_.txBytes;
    if (b == 13 || b == 10) { st_.txCrLf = true; return; }
    if (b < 32 || b > 126) { st_.txBinary = true; return; }
    if (st_.txText.size() < 48) st_.txText += static_cast<char>(b);
    else st_.txTrunc = true;
}

std::string quoted() {
    return "\"" + st_.txText + (st_.txTrunc ? "...\"" : "\"");
}

// Closes the open data transfer and writes its summary line
void closeTx(std::uint64_t t, const char* how) {
    if (st_.tx == Tx::None) return;
    const std::string dur = durText(t - st_.txStart);
    char b[160];
    if (st_.tx == Tx::CtrlSend) {
        const std::string to = listenerNames(st_.listeners);
        std::string what;
        if (st_.pendingDdl >= 0 && listenerIsDrive()) {
            std::snprintf(b, sizeof(b), "%s %d bytes (%d records)", driveDdlName(st_.pendingDdl),
                          st_.txBytes, (st_.txBytes + 255) / 256);
            what = b;
        } else if (st_.txBinary || st_.txText.empty()) {
            std::snprintf(b, sizeof(b), "%d bytes", st_.txBytes);
            what = b;
        } else {
            std::snprintf(b, sizeof(b), " (%d bytes%s)", st_.txBytes, st_.txCrLf ? ", CR LF" : "");
            what = quoted() + b;
        }
        emit(st_.txStart, kWhite, st_.ctrl + " -> " + to + "   " + what + "   " + dur);
    } else {
        const std::string from = nameOf(st_.talker);
        std::string what;
        if (st_.txReq == SST) {
            const int s = st_.ddlArgs.empty() ? -1 : st_.ddlArgs[0];
            if (s < 0) what = "status: -";
            else {
                std::snprintf(b, sizeof(b), "status %d%s%s", s, talkerIsDrive() ? " " : "",
                              talkerIsDrive() ? driveStatus(s) : "");
                what = b;
            }
        } else if (st_.txReq == SAI) {
            const int a = st_.ddlArgs.empty() ? -1 : st_.ddlArgs[0];
            std::snprintf(b, sizeof(b), "accessory ID 0x%02X (%s)", a & 0xFF, aidClass(static_cast<std::uint8_t>(a)));
            what = b;
        } else if (st_.txReq == SDI) {
            what = "device ID " + quoted();
        } else if (st_.pendingDdt >= 0 && talkerIsDrive()) {
            if (st_.pendingDdt == 3 && st_.ddlArgs.size() >= 3) {
                std::snprintf(b, sizeof(b), "POSITION track %d record %d byte %d", st_.ddlArgs[0], st_.ddlArgs[1], st_.ddlArgs[2]);
            } else {
                std::snprintf(b, sizeof(b), "%s %d bytes (%d records)", driveDdtName(st_.pendingDdt),
                              st_.txBytes, (st_.txBytes + 255) / 256);
            }
            what = b;
        } else if (st_.txBinary || st_.txText.empty()) {
            std::snprintf(b, sizeof(b), "%d bytes", st_.txBytes);
            what = b;
        } else {
            std::snprintf(b, sizeof(b), " (%d bytes)", st_.txBytes);
            what = quoted() + b;
        }
        if (how) what += how;
        emit(st_.txStart, std::strstr(what.c_str(), "ERROR") ? kRed : kCyan,
             st_.ctrl + " <- " + from + "   " + what + "   " + dur);
    }
    st_.tx = Tx::None;
    st_.txBytes = 0;
    st_.txText.clear();
    st_.txBinary = st_.txCrLf = st_.txTrunc = false;
    st_.ddlArgs.clear();
}

void startTx(Tx kind, std::uint64_t t) {
    st_.tx = kind;
    st_.txStart = t;
    st_.txBytes = 0;
    st_.txText.clear();
    st_.txBinary = st_.txCrLf = st_.txTrunc = false;
    st_.txAnswered = false;
    st_.ddlArgs.clear();
}

// ── Overview: frame -> transactions ─────────────────────────────────────
void overviewFrame(std::uint64_t t, IL_CMD_t in, IL_CMD_t out, std::uint32_t count) {
    const bool absorbed = out == IL_NO_FRAME;

    if (IS_IDLE(in)) { st_.idle += count; return; }

    // A long pause inside a running transfer
    if (st_.tx != Tx::None && t - st_.lastT > kSlowUs) {
        emit(t, kRed, "SLOW: " + durText(t - st_.lastT) + " pause during transfer");
    }

    // HIPI's talker ends its transfer: the echo of its last byte comes
    // back and is replaced by ETO/ETE (not another byte)
    if (!absorbed && IS_DATA(in) && in != out && !IS_DATA(out)) {
        if (out == ETE) closeTx(t, "   ERROR (ETE)");
        else            closeTx(t, nullptr);
        st_.lastT = t;
        return;
    }

    if (IS_DATA(in)) {
        // The byte: what HIPI's talker put on the loop (it replaced the
        // echo of its previous byte), else what arrived
        const int byte = (!absorbed && in != out && IS_DATA(out)) ? (out & 0xFF) : (in & 0xFF);
        if (st_.tx == Tx::DevSend) {
            st_.txAnswered = true;
            if (st_.txReq != SDA || st_.pendingDdt == 3) st_.ddlArgs.push_back(byte);
            // Remember accessory IDs (also of devices outside HIPI) for names
            if (st_.txReq == SAI && st_.ddlArgs.size() == 1 && st_.talker >= 0 && st_.talker < 32) {
                st_.aid[st_.talker] = static_cast<std::uint8_t>(byte);
                st_.haveAid[st_.talker] = true;
            }
            addByte(static_cast<std::uint8_t>(byte));
        } else if (st_.pendingDdl == 4 || st_.pendingDdl == 3) {
            // SEEK: track + record / SET BYTE POINTER: the pointer
            st_.ddlArgs.push_back(byte);
            if (st_.pendingDdl == 4 && st_.ddlArgs.size() == 2) {
                char b[80];
                std::snprintf(b, sizeof(b), "SEEK track %d record %d", st_.ddlArgs[0], st_.ddlArgs[1]);
                emitIdle(t);
                emit(t, kYellow, listenerNames(st_.listeners) + "   " + b);
                st_.ddlArgs.clear();
                st_.pendingDdl = -1;
            }
        } else {
            if (st_.tx == Tx::None) { emitIdle(t); startTx(Tx::CtrlSend, t); }
            addByte(static_cast<std::uint8_t>(byte));
        }
        st_.lastT = t;
        return;
    }

    // A request that got no answer: the frame after it isn't data
    if (st_.tx == Tx::DevSend && !st_.txAnswered && !(IS_DATA(in))) {
        if (in != ETO && in != ETE && in != NRD) {
            emitIdle(t);
            emit(st_.txStart, kRed, "NO RESPONSE: " + mnem(st_.txReq) + " to " + nameOf(st_.talker) +
                                    (st_.talker < 0 ? " (no talker addressed)" : ""));
            st_.tx = Tx::None;
        }
    }

    if (IS_CMD(in)) {
        if (st_.tx != Tx::None) closeTx(t, nullptr);
        const int a = in & 0x1F;
        if (in >= LAD && in < UNL)       { st_.listeners |= 1u << a; }
        else if (in == UNL)              { st_.listeners = 0; st_.pendingDdl = -1; }
        else if (in >= TAD && in < UNT)  { st_.talker = a; st_.pendingDdt = -1; }
        else if (in == UNT)              { st_.talker = -1; st_.pendingDdt = -1; }
        else if (in == IFC)              { emitIdle(t); emit(t, kYellow, "INTERFACE CLEAR");
                                           st_.talker = -1; st_.listeners = 0; st_.pendingDdl = st_.pendingDdt = -1; }
        else if (in == DCL)              { emitIdle(t); emit(t, kYellow, "DEVICE CLEAR (all devices)"); }
        else if (in == SDC)              { emitIdle(t); emit(t, kYellow, "DEVICE CLEAR -> " + listenerNames(st_.listeners)); }
        else if (in == LPD)              { emitIdle(t); emit(t, kYellow, "LOOP POWER DOWN"); }
        else if (in == AAU)              { /* reported with the AAD that follows */ }
        else if (in >= DDL && in < DDL + 32) {
            st_.pendingDdl = a;
            st_.ddlArgs.clear();
            if (listenerIsDrive() && a != 4 && a != 3) {
                emitIdle(t);
                emit(t, kYellow, listenerNames(st_.listeners) + "   " + driveDdlName(a));
            } else if (!listenerIsDrive()) {
                emitIdle(t);
                emit(t, kGrey, listenerNames(st_.listeners) + "   " + mnem(in));
            }
        }
        else if (in >= DDT && in < DDT + 32) {
            st_.pendingDdt = a;
            if (talkerIsDrive()) { emitIdle(t); emit(t, kYellow, nameOf(st_.talker) + "   " + driveDdtName(a)); }
        }
        else { emitIdle(t); emit(t, kGrey, mnem(in)); }
        st_.lastCmd = in;
        st_.lastCmdT = t;
        st_.lastT = t;
        return;
    }

    // Ready group
    if (in == RFC) {
        // The command before it took long to be accepted
        if (t - st_.lastCmdT > kSlowUs && st_.lastCmd != 0) {
            emit(t, kRed, "SLOW: " + mnem(st_.lastCmd) + " took " + durText(t - st_.lastCmdT));
        }
        st_.lastCmd = 0;
    } else if (in >= AAD && in < AAD + 32) {
        const int first = in & 0x1F;
        const int next = (!absorbed && out >= AAD && out < AAD + 32) ? (out & 0x1F) : first;
        std::string s = "AUTO-ADDRESS";
        char b[64];
        if (first > 1) { std::snprintf(b, sizeof(b), "   before HIPI: %d device(s)", first - 1); s += b; }
        // HIPI's own devices first; any other address in HIPI's range was
        // taken by a device on the PC behind PILBOX
        st_.pcAddrs = 0;
        std::string pcList;
        s += "   HIPI:";
        for (int x = first; x < next && x < 31; ++x) {
            if (hipiDeviceAt(x) != nullptr) {
                s += " " + nameOf(x);
            } else {
                st_.pcAddrs |= 1u << x;
                pcList += " " + nameOf(x);
            }
        }
        if (next == first) s += " (none)";
        if (!pcList.empty()) {
            // nameOf() already says "PC addr n" -- list them without it
            std::string plain;
            for (int x = first; x < next && x < 31; ++x) {
                if (!(st_.pcAddrs & (1u << x))) continue;
                char pb[40];
                if (st_.haveAid[x]) std::snprintf(pb, sizeof(pb), " addr %d (%s)", x, aidClass(st_.aid[x]));
                else                std::snprintf(pb, sizeof(pb), " addr %d", x);
                plain += pb;
            }
            s += "   PC (via PILBOX):" + plain;
        }
        emitIdle(t);
        emit(t, kYellow, s);
    } else if (in == SDA || in == SST || in == SAI || in == SDI) {
        emitIdle(t);
        startTx(Tx::DevSend, t);
        st_.txReq = in;
        if (!absorbed && in != out && IS_DATA(out)) {
            st_.txAnswered = true;
            const int byte = out & 0xFF;
            if (in != SDA || st_.pendingDdt == 3) st_.ddlArgs.push_back(byte);
            addByte(static_cast<std::uint8_t>(byte));
            if (in == SAI && st_.talker >= 0 && st_.talker < 32) {
                st_.aid[st_.talker] = static_cast<std::uint8_t>(byte);
                st_.haveAid[st_.talker] = true;
            }
        }
    } else if (in == ETO || (!absorbed && out == ETO)) {
        closeTx(t, nullptr);
    } else if (in == ETE || (!absorbed && out == ETE)) {
        closeTx(t, "   ERROR (ETE)");
    } else if (in == NRD) {
        closeTx(t, nullptr);
    } else if (in == 0x564) {                   // TCT: TAKE CONTROL
        emitIdle(t);
        emit(t, kYellow, "TAKE CONTROL: " + st_.ctrl + " -> " + nameOf(st_.talker));
        if (st_.talker >= 0) st_.ctrl = "CTRL " + nameOf(st_.talker);
    } else {
        emitIdle(t);
        emit(t, kGrey, mnem(in));
    }
    if (absorbed) emit(t, kYellow, "frame absorbed -- a device is powering up");
    st_.lastT = t;
}

// ── Detailed: one line per frame ────────────────────────────────────────
void detailedFrame(std::uint64_t t, IL_CMD_t in, IL_CMD_t out, std::uint32_t count) {
    if (IS_IDLE(in) && !showIdle_) return;
    const bool absorbed = out == IL_NO_FRAME;
    char b[160];
    std::string s = mnem(in);
    s.resize(8, ' ');
    if (count > 1) {                        // a run of identical idle frames
        std::snprintf(b, sizeof(b), "x %lu (idle polling)", static_cast<unsigned long>(count));
        emit(t, kGrey, s + b);
        return;
    }
    const int a = in & 0x1F;
    if (IS_DATA(in)) {
        const int c = in & 0xFF;
        std::snprintf(b, sizeof(b), "0x%02X '%c'", c, (c >= 32 && c < 127) ? c : '.');
        s += b;
    } else if (in >= LAD && in < UNL) s += "listen: " + nameOf(a);
    else if (in >= TAD && in < UNT)   s += "talk: " + nameOf(a);
    else if (in >= DDL && in < DDL + 32 && listenerIsDrive()) s += driveDdlName(a);
    else if (in >= DDT && in < DDT + 32 && talkerIsDrive())   s += driveDdtName(a);

    std::uint16_t color = IS_DATA(in) ? kWhite : IS_IDLE(in) ? kGrey : kYellow;
    if (absorbed) {
        s += "   absorbed (device powering up)";
        color = kRed;
    } else if (out != in) {
        s += "   -> " + mnem(out);
        if (IS_DATA(out)) {
            const int c = out & 0xFF;
            std::snprintf(b, sizeof(b), " 0x%02X '%c'", c, (c >= 32 && c < 127) ? c : '.');
            s += b;
        }
        s += IS_DATA(out) && st_.talker >= 0 ? "  (HIPI: " + nameOf(st_.talker) + ")" : std::string("  (HIPI)");
        color = kGreen;
    }
    if (in == ETE || out == ETE) color = kRed;
    emit(t, color, s);
    // Keep addressing state for names
    if (in >= LAD && in < UNL) st_.listeners |= 1u << a;
    else if (in == UNL) st_.listeners = 0;
    else if (in >= TAD && in < UNT) st_.talker = a;
    else if (in == UNT || in == IFC) { st_.talker = -1; if (in == IFC) st_.listeners = 0; }
}

void resetAnalysis() {
    st_ = State();
    lines_.clear();
    scroll_ = 0;
    dirty_ = true;
}

void analyseEntry(std::uint64_t t, const Capture& c) {
    // A run of identical idle frames (see flushRun()) is one entry
    const std::uint32_t count = isRun(c) ? (c.out & ~kRunFlag) : 1;
    const IL_CMD_t out = isRun(c) ? c.in : c.out;
    if (mode_ == AnalyzerMode::Overview) overviewFrame(t, c.in, out, count);
    else                                 detailedFrame(t, c.in, out, count);
}

// Full-time (64-bit, µs since start-up) of a captured 32-bit timestamp
std::uint64_t fullTime(std::uint32_t tUs) {
    const std::uint64_t now = time_us_64();
    return now - static_cast<std::uint32_t>(static_cast<std::uint32_t>(now) - tUs);
}

// Re-runs the analysis over everything still in the ring (mode change)
void reanalyse() {
    resetAnalysis();
    const std::uint32_t head = head_;
    const std::uint32_t first = head > kRingSize ? head - kRingSize : 0;
    for (std::uint32_t i = first; i < head; ++i) {
        const Capture& c = ring_[i % kRingSize];
        analyseEntry(fullTime(c.tUs), c);
    }
    analysed_ = head;
}

// ── Drawing ─────────────────────────────────────────────────────────────
constexpr int kTitleH = 20;
constexpr int kRowH = 16;
constexpr int kRows = (SCREEN_MAX_Y - kTitleH - 4) / kRowH;
constexpr int kCols = (SCREEN_MAX_X - 16) / 8;

bool visible() {
    if (plotterview_output() != DisplayOutput::Analyzer || plotterview_isSplashVisible()) return false;
#ifndef DISPLAY_7INCH
    if (boardui_isMenuOpen()) return false;    // 5": the menu is drawn on the panel itself
#endif
    return true;
}

void drawText(int x, int y, const std::string& s, std::uint16_t fg, std::uint16_t bg) {
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s.c_str());
}

void draw() {
    if (!lines_.empty()) backlight_activity();   // new lines shown (backlight.h)
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    d_->txtSize(0);
    // Title bar
    d_->fillRect(0, 0, SCREEN_MAX_X, kTitleH, kYellow);
    char b[160];
    std::snprintf(b, sizeof(b), " HP-IL ANALYZER   %s   time: %s   idle: %s   log: %s   frames: %lu%s%s",
                  mode_ == AnalyzerMode::Overview ? "OVERVIEW" : "DETAILED",
                  relative_ ? "relative" : "since start-up", showIdle_ ? "shown" : "hidden",
                  logging_ ? logName_.c_str() : "off", static_cast<unsigned long>(frames_),
                  paused_ ? "   [PAUSED]" : "", scroll_ > 0 ? "   [SCROLLED]" : "");
    drawText(4, 2, b, 0x0000, kYellow);
    // Lines
    d_->fillRect(0, kTitleH, SCREEN_MAX_X, SCREEN_MAX_Y - kTitleH, 0x0000);
    // Lines too long for the screen are wrapped (at a space where
    // possible); the continuation rows are indented past the time column.
    // Filled from the bottom up: the newest line shown (scroll_ lines back
    // from the newest) ends on the last row.
    struct Row { std::string time; std::string text; std::size_t indent; std::uint16_t color; };
    std::vector<Row> rows;                       // newest first
    const int n = static_cast<int>(lines_.size());
    for (int i = n - 1 - scroll_; i >= 0 && static_cast<int>(rows.size()) < kRows; --i) {
        const Line& l = lines_[static_cast<std::size_t>(i)];
        const std::uint64_t prevT = i > 0 ? lines_[static_cast<std::size_t>(i - 1)].t : l.t;
        const std::string tt = timeText(l.t, prevT) + "  ";   // grey time column
        const std::size_t width = kCols > static_cast<int>(tt.size()) + 8
                                  ? static_cast<std::size_t>(kCols) - tt.size() : 8;
        std::vector<std::string> parts;
        std::string rest = l.text;
        while (rest.size() > width) {
            std::size_t cut = rest.rfind(' ', width);
            if (cut == std::string::npos || cut < width / 2) cut = width;   // no good space: hard cut
            parts.push_back(rest.substr(0, cut));
            rest = rest.substr(cut);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        }
        parts.push_back(rest);
        for (std::size_t k = parts.size(); k-- > 0;) {
            rows.push_back(Row{ k == 0 ? tt : std::string(), parts[k], tt.size(), l.color });
        }
    }
    const int shown = std::min(static_cast<int>(rows.size()), kRows);
    for (int r = 0; r < shown; ++r) {
        const Row& row = rows[static_cast<std::size_t>(r)];
        const int y = kTitleH + 4 + (kRows - 1 - r) * kRowH;
        if (!row.time.empty()) drawText(4, y, row.time, kGrey, 0x0000);
        drawText(4 + static_cast<int>(row.indent) * 8, y, row.text, row.color, 0x0000);
    }
    if (paused_) {
        // Banner at the top while paused (the screen is frozen underneath)
        const char* msg = " PAUSED -- touch the screen to resume ";
        const int w = static_cast<int>(std::strlen(msg)) * 16 + 16;
        const int x = (SCREEN_MAX_X - w) / 2;
        d_->fillRect(x, kTitleH + 8, w, 44, kRed);
        d_->txtSize(1);
        drawText(x + 8, kTitleH + 14, msg, kWhite, kRed);
        d_->txtSize(0);
    }
    dirty_ = false;
}

}  // namespace

void analyzer_init(DisplayDriver* display) {
    d_ = display;
}

void analyzer_capture(IL_CMD_t in, IL_CMD_t out) {
    const std::uint32_t now = time_us_32();
    ++frames_;
    if (IS_IDLE(in) && in == out) {
        // Idle polling passed on unchanged: count it into the current run
        if (runCount_ > 0 && runIn_ == in && runCount_ < kRunMax) {
            ++runCount_;
            runLastT_ = now;
            return;
        }
        flushRun();
        runIn_ = in;
        runCount_ = 1;
        runT_ = runLastT_ = now;
        return;
    }
    flushRun();
    push(now, in, out);
}

void analyzer_poll() {
    // An idle run that has gone quiet is stored now, so it shows up
    if (runCount_ > 0 && time_us_32() - runLastT_ > 500000) flushRun();
    const std::uint32_t head = head_;
    if (head - analysed_ > kRingSize) {
        lost_ += head - analysed_ - kRingSize;
        analysed_ = head - kRingSize;
        char b[48];
        std::snprintf(b, sizeof(b), "... %lu frames lost (too fast)", static_cast<unsigned long>(lost_));
        emit(time_us_64(), kRed, b);
    }
    // A bounded amount per call, so the main loop stays responsive
    for (int n = 0; analysed_ < head && n < 64; ++n, ++analysed_) {
        const Capture& c = ring_[analysed_ % kRingSize];
        analyseEntry(fullTime(c.tUs), c);
    }
    if (((dirty_ && !paused_) || forceDraw_) && d_ != nullptr && visible() && time_reached(nextDraw_)) {
        forceDraw_ = false;
        nextDraw_ = make_timeout_time_ms(kDrawIntervalMs);
        draw();
    }
}

void analyzer_redrawAll() {
    if (d_ != nullptr) draw();
}

void analyzer_scroll(int n) {
    const int maxScroll = std::max(0, static_cast<int>(lines_.size()) - kRows);
    scroll_ = std::max(0, std::min(maxScroll, scroll_ + n));
    if (d_ != nullptr && visible()) draw();
}

void analyzer_scrollToLive() {
    scroll_ = 0;
    if (d_ != nullptr && visible()) draw();
}

int analyzer_pageRows() { return kRows; }

void analyzer_clear() {
    head_ = 0;
    frames_ = 0;
    runCount_ = 0;
    analysed_ = 0;
    lost_ = 0;
    resetAnalysis();
    // Redrawn by analyzer_poll() right away -- NOT drawn here: this is
    // called from the menu, where the 7" panel's canvas is the menu layer
    // (drawing now would land under the menu box, and then clear dirty_)
    nextDraw_ = nil_time;
}

void analyzer_setMode(AnalyzerMode m) {
    if (m == mode_) return;
    mode_ = m;
    reanalyse();
}
AnalyzerMode analyzer_mode() { return mode_; }

void analyzer_setRelativeTime(bool r) { relative_ = r; dirty_ = true; }
bool analyzer_relativeTime() { return relative_; }

void analyzer_setShowIdle(bool s) {
    if (s == showIdle_) return;
    showIdle_ = s;
    reanalyse();
}
bool analyzer_showIdle() { return showIdle_; }

void analyzer_setPaused(bool p) {
    paused_ = p;
    // One redraw either way (from analyzer_poll(), on the main canvas):
    // pausing shows the PAUSED banner and then freezes the screen,
    // resuming catches up with everything that arrived meanwhile
    dirty_ = true;
    forceDraw_ = true;
    nextDraw_ = nil_time;
}
bool analyzer_paused() { return paused_; }

// Creates the next free logs/analyzer_<n>.txt and writes its header line
static bool openNewLogFile(FIL& file, std::string& name, std::string& message, const char* what) {
    if (usbMscModeActive()) { message = "SD card in use by PC"; return false; }
    f_mkdir(HIPI_DIR_LOGS);
    char path[48];
    FILINFO fi;
    for (unsigned n = 0; n < 10000; ++n) {
        std::snprintf(path, sizeof(path), HIPI_DIR_LOGS "/analyzer_%u.txt", n);
        if (f_stat(path, &fi) == FR_NO_FILE) break;
    }
    if (f_open(&file, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        message = "Can't create log file";
        return false;
    }
    const std::string head = std::string("HIPI HP-IL analyzer log -- ") + what + ", " +
                             (mode_ == AnalyzerMode::Overview ? "overview" : "detailed") + "\r\n";
    UINT bw = 0;
    f_write(&file, head.data(), static_cast<UINT>(head.size()), &bw);
    name = path;
    message = path;
    return true;
}

bool analyzer_startLog(std::string& message) {
    if (logging_) { message = logName_; return true; }
    if (!openNewLogFile(logFile_, logName_, message, "logging")) return false;
    logging_ = true;
    logUnsynced_ = 0;
    dirty_ = true;
    LOGF("\r\n * Analyzer: logging to %s", logName_.c_str());
    return true;
}

bool analyzer_saveLog(std::string& message) {
    if (logging_) { message = "Logging is on -- stop it first"; return false; }
    if (lines_.empty()) { message = "Nothing to save"; return false; }
    FIL file;
    std::string name;
    if (!openNewLogFile(file, name, message, "saved buffer")) return false;
    // Everything still in the view's buffer, as shown
    std::uint64_t prevT = lines_.front().t;
    for (const Line& l : lines_) {
        const std::string s = timeText(l.t, prevT) + "  " + l.text + "\r\n";
        prevT = l.t;
        UINT bw = 0;
        f_write(&file, s.data(), static_cast<UINT>(s.size()), &bw);
    }
    f_close(&file);
    LOGF("\r\n * Analyzer: %u lines saved to %s", static_cast<unsigned>(lines_.size()), name.c_str());
    return true;
}

void analyzer_stopLog() {
    if (!logging_) return;
    f_close(&logFile_);
    logging_ = false;
    dirty_ = true;
    LOGF("\r\n * Analyzer: log %s closed", logName_.c_str());
}

bool analyzer_logging() { return logging_; }

}  // namespace hipi
