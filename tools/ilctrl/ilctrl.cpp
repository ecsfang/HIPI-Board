// ilctrl.cpp -- a small command-line HP-IL loop controller for Linux,
// talking to a PIL-Box (or HIPI's PILBOX) in CON mode over a serial port.
//
// A C++ port of the ideas in J-F Garnier's ILCtrl 1.05 (Visual Basic, GPL),
// http://www.jeffcalc.hp41.eu/hpil/index.html -- same PIL-Box protocol,
// same HP-IL transactions, a text interface instead of the window.
//
// Build:   g++ -std=c++17 -O2 -Wall -o ilctrl ilctrl.cpp
// Run:     ./ilctrl /dev/ttyACM1               (interactive; "help" lists commands)
//          ./ilctrl /dev/ttyACM1 -c "did 1; scan"   (commands from the command line)
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2 of the License, or (at your
// option) any later version.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

// ── HP-IL frames ────────────────────────────────────────────────────────
namespace il {
// command frames
constexpr int GET = 0x408, DCL = 0x414, LAD = 0x420, UNL = 0x43F, TAD = 0x440, UNT = 0x45F;
constexpr int IFC = 0x490, REN = 0x492, NRE = 0x493, AAU = 0x49A;
// ready frames
constexpr int RFC = 0x500, ETO = 0x540, ETE = 0x541, NRD = 0x542;
constexpr int SDA = 0x560, SST = 0x561, SDI = 0x562, SAI = 0x563, AAD = 0x580;
// PIL-Box commands
constexpr int TDIS = 0x494, COFI = 0x495, CON = 0x496, COFF = 0x497;
}  // namespace il

// Frame -> mnemonic (as ILCtrl's ilscope.vb)
static std::string mnemo(int frame) {
    static const char* cls[8] = { "DAB", "DSR", "END", "ESR", "CMD", "RDY", "IDY", "ISR" };
    static const char* scmd0[32] = {
        "NUL", "GTL", 0, 0, "SDC", "PPD", 0, 0, "GET", 0, 0, 0, 0, 0, 0, "ELN",
        "NOP", "LLO", 0, 0, "DCL", "PPU", 0, 0, "EAR", 0, 0, 0, 0, 0, 0, 0 };
    static const char* scmd9[16] = { "IFC", 0, "REN", "NRE", 0, 0, 0, 0, 0, 0, "AAU", "LPD", 0, 0, 0, 0 };
    char b[16];
    const int n = frame & 0xFF;
    std::snprintf(b, sizeof(b), "%s %02X", cls[(frame >> 8) & 7], n);
    std::string s = b;
    auto hex = [](const char* p, int v) { char t[16]; std::snprintf(t, sizeof(t), "%s %02X", p, v); return std::string(t); };
    if ((frame & 0x700) == 0x400) {
        const char* p = nullptr;
        switch (n / 32) {
            case 0: p = scmd0[n & 31]; if (p) s = p; break;
            case 1: s = (n & 31) == 31 ? "UNL" : hex("LAD", n & 31); p = ""; break;
            case 2: s = (n & 31) == 31 ? "UNT" : hex("TAD", n & 31); p = ""; break;
            case 3: s = hex("SAD", n & 31); p = ""; break;
            case 4: if ((n & 31) < 16) { s = hex("PPE", n & 31); p = ""; } else { p = scmd9[n & 15]; if (p) s = p; } break;
            case 5: s = hex("DDL", n & 31); p = ""; break;
            case 6: s = hex("DDT", n & 31); p = ""; break;
        }
        if (p == nullptr) s = hex("CMD", n);
    } else if ((frame & 0x700) == 0x500) {
        if (n < 128) {
            switch (n) {
                case 0: s = "RFC"; break;   case 64: s = "ETO"; break; case 65: s = "ETE"; break;
                case 66: s = "NRD"; break;  case 96: s = "SDA"; break; case 97: s = "SST"; break;
                case 98: s = "SDI"; break;  case 99: s = "SAI"; break; case 100: s = "TCT"; break;
                default: s = hex("RDY", n);
            }
        } else {
            switch (n / 32) {
                case 4: s = hex("AAD", n & 31); break; case 5: s = hex("AEP", n & 31); break;
                case 6: s = hex("AES", n & 31); break; case 7: s = hex("AMP", n & 31); break;
                default: s = hex("RDY", n);
            }
        }
    }
    return s;
}

// ── The serial link to the PIL-Box ──────────────────────────────────────
class PilBox {
public:
    bool open(const std::string& dev) {
        fd_ = ::open(dev.c_str(), O_RDWR | O_NOCTTY);
        if (fd_ < 0) { std::perror(dev.c_str()); return false; }
        termios t{};
        tcgetattr(fd_, &t);
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 1;                       // read() waits up to 0.1 s
        tcsetattr(fd_, TCSANOW, &t);
        int bits = TIOCM_DTR | TIOCM_RTS;        // "port open" for a USB CDC device (HIPI)
        ioctl(fd_, TIOCMBIS, &bits);
        usleep(200 * 1000);
        tcflush(fd_, TCIOFLUSH);
        return true;
    }
    void close() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
    ~PilBox() { close(); }

    bool scopeOut = false, scopeIn = false;   // show frames sent / received

    // Sends a frame, waits for the frame that comes back (-1: timeout) --
    // as ILCtrl's SendFrame(): the high byte only when it changed, timeouts
    // by frame type
    int sendFrame(int frame) {
        if (frame < 0) return -1;
        const int hbyt = ((frame >> 6) & 0x1E) | 0x20;      // 8-bit format
        const int lbyt = (frame & 0x7F) | 0x80;
        std::uint8_t buf[2];
        int n = 0;
        if (hbyt != lasth_) { lasth_ = hbyt; buf[n++] = static_cast<std::uint8_t>(hbyt); }
        buf[n++] = static_cast<std::uint8_t>(lbyt);
        tcflush(fd_, TCIFLUSH);                               // stale input (as ILCtrl 1.01)
        if (::write(fd_, buf, static_cast<size_t>(n)) != n) { std::perror("write"); return -1; }
        if (scopeOut) std::printf("  --> %-8s (%03X)\n", mnemo(frame).c_str(), frame);
        int timeoutMs;
        if (frame < 0x400)                  timeoutMs = 10000;   // DOE
        else if ((frame & 0x7F4) == 0x494)  timeoutMs = 500;     // PIL-Box commands
        else if (frame < 0x500)             timeoutMs = 3000;    // CMD
        else if (frame < 0x600)             timeoutMs = 10000;   // RDY
        else                                timeoutMs = 1000;    // IDY
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int result = -1;
        while (result < 0 && std::chrono::steady_clock::now() < deadline) {
            std::uint8_t byt;
            const ssize_t r = ::read(fd_, &byt, 1);
            if (r <= 0 || byt == 0) continue;
            if ((byt & 0xC0) == 0) {
                if (byt & 0x20) lasth_ = byt;                 // high byte
            } else if (byt & 0x80) {
                result = ((lasth_ & 0x1E) << 6) + (byt & 0x7F);
            } else {
                result = ((lasth_ & 0x1F) << 6) + (byt & 0x3F);
            }
        }
        if (result >= 0 && scopeIn) std::printf("  <-- %-8s (%03X)\n", mnemo(result).c_str(), result);
        if (result < 0 && (scopeIn || scopeOut)) std::printf("  <-- (timeout after %d ms)\n", timeoutMs);
        return result;
    }

    // PIL-Box command (TDIS, CON...): the box answers with the same frame
    bool init(int cmd) {
        lasth_ = 0;
        return sendFrame(cmd) == cmd;
    }
    void resync() { lasth_ = 0; }

private:
    int fd_ = -1;
    int lasth_ = 0;
};

// ── HP-IL transactions (as ILCtrl) ──────────────────────────────────────
class Controller {
public:
    explicit Controller(PilBox& box) : box_(box) {}

    // IFC (optionally), AAU, AAD 1 -- the loop in a known state, addressed
    bool startTrans(bool doIfc) {
        box_.resync();
        if (doIfc && box_.sendFrame(il::IFC) != il::IFC) return false;
        if (box_.sendFrame(il::AAU) != il::AAU) return false;
        const int r = box_.sendFrame(il::AAD + 1);
        if (r < 0) return false;
        devices_ = (r >= il::AAD && r < il::AAD + 32) ? (r - il::AAD - 1) : -1;
        return true;
    }
    void endTrans() {
        box_.resync();
        box_.sendFrame(il::UNL);
        box_.sendFrame(il::UNT);
    }
    int devices() const { return devices_; }          // from the last AAD

    bool sendUCG(int frame) {                          // universal command
        if (!startTrans(true)) return err();
        box_.sendFrame(frame);
        return true;
    }
    bool sendACG(int addr, int frame) {                // addressed command
        if (!startTrans(false)) return err();
        box_.sendFrame(il::LAD + addr);
        box_.sendFrame(frame);
        endTrans();
        return true;
    }
    bool sendData(int addr, const std::string& s) {
        if (!startTrans(false)) return err();
        box_.sendFrame(il::LAD + addr);
        for (unsigned char c : s)
            if (box_.sendFrame(c) < 0) return err();
        endTrans();
        return true;
    }
    // Text from a talker (SDA or SDI); stops at LF, END byte, 256 bytes.
    // Returns the byte count, -1 on error, -2 if nobody answered.
    int getData(int addr, int sot, std::string& s) {
        s.clear();
        int i = 0;
        if (!startTrans(false)) return err(), -1;
        box_.sendFrame(il::TAD + addr);
        int frame = box_.sendFrame(sot);
        if (frame < 0) return err(), -1;
        if (frame != sot) {
            for (;;) {
                if (frame == il::ETO || frame == il::ETE) break;
                if (frame < 0x400) {
                    s += static_cast<char>(frame & 0xFF);
                    ++i;
                    if ((frame & 0xFF) == 10 || (frame & 0x200) || i > 256) {
                        if (box_.sendFrame(il::NRD) != il::NRD) return err(), -1;   // stop the transfer
                    }
                }
                frame = box_.sendFrame(frame);        // pass it on, get the next
                if (frame < 0) break;
            }
        }
        endTrans();
        if (frame == il::ETE || frame < 0) return err(), -1;
        if (frame == sot) return -2;
        return i;
    }
    // Bytes from a talker (SDA, SAI, SST), at most n
    int getBytes(int addr, int sot, std::vector<int>& out, int n) {
        out.clear();
        if (!startTrans(false)) return err(), -1;
        box_.sendFrame(il::TAD + addr);
        int frame = box_.sendFrame(sot);
        if (frame < 0) return err(), -1;
        if (frame != sot) {
            for (;;) {
                if (frame == il::ETO || frame == il::ETE) break;
                if (frame < 0x400) {
                    out.push_back(frame & 0xFF);
                    if (static_cast<int>(out.size()) >= n && box_.sendFrame(il::NRD) != il::NRD) return err(), -1;
                }
                frame = box_.sendFrame(frame);
                if (frame < 0) break;
            }
        }
        endTrans();
        if (frame == il::ETE || frame < 0) return err(), -1;
        if (frame == sot) return -2;
        return static_cast<int>(out.size());
    }

private:
    bool err() { std::printf("TRANSMIT ERR\n"); return false; }
    PilBox& box_;
    int devices_ = -1;
};

// ── Command line interface ──────────────────────────────────────────────
static const char* kHelp =
    "Commands (addr defaults to the current address, see 'addr'):\n"
    "  addr <n>          set the current device address (1..30)\n"
    "  did [addr]        device ID (SDI)\n"
    "  aid [addr]        accessory ID (SAI)\n"
    "  spoll [addr]      serial poll: status bytes (SST)\n"
    "  enter [addr]      read text from the device (SDA)\n"
    "  send <addr> <text>  send text (+ CR LF) to the device\n"
    "  trigger [addr]    GET to the device\n"
    "  remote | local    REN / NRE to all devices\n"
    "  clear             DCL: clear all devices\n"
    "  scan              auto-address the loop, then ID and type of every device\n"
    "  frame <hex>       send one raw frame (e.g. 'frame 490' = IFC), show what comes back\n"
    "  scope off|out|in|both   show frames sent/received\n"
    "  con | coff | tdis   PIL-Box mode commands\n"
    "  help | quit\n";

static int parseInt(const std::string& s, int dflt, int base = 10) {
    if (s.empty()) return dflt;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, base);
    return (end && *end == '\0') ? static_cast<int>(v) : dflt;
}

static std::string printable(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();   // the line end
    std::string r;
    for (unsigned char c : s) {
        if (c == '\r') continue;
        if (c == '\n') { r += "\\n"; continue; }
        if (c < 32 || c > 126) { char b[8]; std::snprintf(b, sizeof(b), "<%02X>", c); r += b; }
        else r += static_cast<char>(c);
    }
    return r;
}

static bool runCommand(const std::string& line, PilBox& box, Controller& ctl, int& addr) {
    std::istringstream in(line);
    std::string cmd, a1;
    in >> cmd;
    if (cmd.empty() || cmd[0] == '#') return true;
    in >> a1;
    auto target = [&]() { int n = parseInt(a1, addr); return n < 1 ? 1 : (n > 31 ? 31 : n); };

    if (cmd == "quit" || cmd == "exit" || cmd == "q") return false;
    if (cmd == "help" || cmd == "?") { std::printf("%s", kHelp); return true; }
    if (cmd == "addr") { addr = target(); std::printf("address %d\n", addr); return true; }
    if (cmd == "con" || cmd == "coff" || cmd == "tdis") {
        const int f = cmd == "con" ? il::CON : cmd == "coff" ? il::COFF : il::TDIS;
        std::printf("%s: %s\n", cmd.c_str(), box.init(f) ? "acknowledged" : "NO ANSWER from the PIL-Box");
        return true;
    }
    if (cmd == "scope") {
        box.scopeOut = a1 == "out" || a1 == "both" || a1 == "on";
        box.scopeIn = a1 == "in" || a1 == "both" || a1 == "on";
        std::printf("scope: out %s, in %s\n", box.scopeOut ? "on" : "off", box.scopeIn ? "on" : "off");
        return true;
    }
    if (cmd == "frame") {
        const int f = parseInt(a1, -1, 16);
        if (f < 0 || f > 0x7FF) { std::printf("frame: give 0..7FF in hex\n"); return true; }
        const int r = box.sendFrame(f);
        if (r < 0) std::printf("%s (%03X) -> no frame back (timeout)\n", mnemo(f).c_str(), f);
        else       std::printf("%s (%03X) -> %s (%03X)\n", mnemo(f).c_str(), f, mnemo(r).c_str(), r);
        return true;
    }
    if (cmd == "did" || cmd == "enter") {
        const int n = target();
        std::string s;
        const int r = ctl.getData(n, cmd == "did" ? il::SDI : il::SDA, s);
        std::printf("%s (%d): %s\n", cmd == "did" ? "Device ID" : "ENTER", n,
                    r == -2 ? "NO RESPONSE" : r < 0 ? "-" : printable(s).c_str());
        return true;
    }
    if (cmd == "aid" || cmd == "spoll") {
        const int n = target();
        std::vector<int> b;
        const int r = ctl.getBytes(n, cmd == "aid" ? il::SAI : il::SST, b, cmd == "aid" ? 1 : 4);
        std::printf("%s (%d):", cmd == "aid" ? "Accessory ID" : "SPOLL", n);
        if (r == -2) std::printf(" NO RESPONSE");
        for (int v : b) std::printf(" %d (0x%02X)", v, v);
        std::printf("\n");
        return true;
    }
    if (cmd == "send") {
        const int n = parseInt(a1, -1);
        std::string text;
        std::getline(in, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        if (n < 1 || n > 31) { std::printf("send <addr> <text>\n"); return true; }
        std::printf("SEND (%d): %s\n", n, text.c_str());
        ctl.sendData(n, text + "\r\n");
        return true;
    }
    if (cmd == "trigger") { const int n = target(); std::printf("TRIGGER %d\n", n); ctl.sendACG(n, il::GET); return true; }
    if (cmd == "remote")  { std::printf("REMOTE\n"); ctl.sendUCG(il::REN); return true; }
    if (cmd == "local")   { std::printf("LOCAL\n");  ctl.sendUCG(il::NRE); return true; }
    if (cmd == "clear")   { std::printf("CLEAR\n");  ctl.sendUCG(il::DCL); return true; }
    if (cmd == "scan") {
        if (!ctl.startTrans(true)) { std::printf("TRANSMIT ERR\n"); return true; }
        ctl.endTrans();
        const int count = ctl.devices();
        if (count < 0) { std::printf("auto-address: no AAD back\n"); return true; }
        std::printf("%d device(s) on the loop\n", count);
        for (int n = 1; n <= count; ++n) {
            std::string id;
            std::vector<int> aid;
            const int r1 = ctl.getData(n, il::SDI, id);
            const int r2 = ctl.getBytes(n, il::SAI, aid, 1);
            std::printf("  %2d: %-16s", n, r1 >= 0 ? printable(id).c_str() : "(no ID)");
            if (r2 > 0) std::printf(" accessory ID 0x%02X", aid[0]);
            std::printf("\n");
        }
        return true;
    }
    std::printf("unknown command '%s' -- 'help' lists them\n", cmd.c_str());
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <serial device> [-c \"cmd; cmd...\"]\n", argv[0]);
        return 1;
    }
    std::string batch;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "-c") == 0) batch = argv[i + 1];

    PilBox box;
    if (!box.open(argv[1])) return 1;
    // A defined start: TDIS, then CON (the PIL-Box answers each with itself)
    box.init(il::TDIS);
    if (!box.init(il::CON)) {
        std::fprintf(stderr, "No answer from the PIL-Box on %s (CON)\n", argv[1]);
        return 1;
    }
    std::printf("PIL-Box on %s in CON mode -- 'help' lists the commands\n", argv[1]);

    Controller ctl(box);
    int addr = 1;
    bool go = true;
    if (!batch.empty()) {
        std::istringstream cmds(batch);
        std::string c;
        while (go && std::getline(cmds, c, ';')) {
            std::printf("> %s\n", c.c_str());
            go = runCommand(c, box, ctl, addr);
        }
    } else {
        std::string line;
        while (go) {
            std::printf("ilctrl> ");
            std::fflush(stdout);
            if (!std::getline(std::cin, line)) break;
            go = runCommand(line, box, ctl, addr);
        }
    }
    box.init(il::TDIS);                        // leave the PIL-Box idle
    return 0;
}
