// pilbox_test.cpp -- the PILBox emulation (src/pilbox.cpp) against a stand-in
// PC (test/pilboxstubs/tusb.h, a simulated clock): HIPI must never stand
// still waiting for the PC. A frame the PC doesn't answer within 2 s is
// dropped and the PILBox goes to TDIS; frames then pass through until the
// PC sends its mode again.
//
//   make pilbox_test && ./pilbox_test   (in tools/hipiview)
#include "pilbox.h"
#include <cstdio>
#include <stdexcept>
#include <string>

bool bTrace = false, bExtTrace = false;
namespace hipi { class LT7683; }
hipi::LT7683* display = nullptr;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
struct Stuck {};
// HIPI waits for the PC in tud_task(): give up after a while -- a hang
constexpr unsigned long long kHang = 2000000;
void watchdog() { if (pilstub::tasks > kHang) throw Stuck{}; }

// The PC (pyILPER) echoes every frame it gets, at once
void echo() {
    watchdog();
    if (!pilstub::toPc.empty()) {
        pilstub::fromPc.insert(pilstub::fromPc.end(), pilstub::toPc.begin(), pilstub::toPc.end());
        pilstub::toPc.clear();
    }
}
// ... after a delay
std::uint64_t echoAt = 0;
void echoLate() {
    watchdog();
    if (pilClockUs() >= echoAt) echo();
}
// ... or closes the port
std::uint64_t closeAt = 0;
void closeLater() {
    watchdog();
    if (pilClockUs() >= closeAt) pilstub::connected = false;
}

// The PC puts the PILBox in a mode (as pyILPER: hi byte, lo byte)
void pcCommand(CPilBox& p, IL_CMD_t frame) {
    pilstub::fromPc.push_back(static_cast<std::uint8_t>(0x20 | ((frame >> 6) & 0x1E)));
    pilstub::fromPc.push_back(static_cast<std::uint8_t>(0x80 | (frame & 0x7F)));
    for (int i = 0; i < 4; ++i) p.idle();
    pilstub::toPc.clear();
}

// A frame from the loop, as hipi_loop() deals with it: hpil(), then poll()
// from the main loop until it's answered or lost
struct Result {
    bool hung = false;
    bool waited = false;                 // hpil() said kWaiting
    bool lost = false;
    IL_CMD_t out = 0;
    std::uint64_t longestCallUs = 0;     // the longest hpil() / poll() call: the main loop stands still that long
    std::uint64_t tookUs = 0;            // from the frame to its answer (or loss)
};
Result frame(CPilBox& p, IL_CMD_t in) {
    Result r;
    pilstub::tasks = 0;
    const std::uint64_t t0 = pilClockUs();
    try {
        IL_CMD_t out = p.hpil(in);
        r.longestCallUs = pilClockUs() - t0;
        if (out == CPilBox::kWaiting) {
            r.waited = true;
            for (;;) {
                pilClockUs() += 5000;                     // (the rest of the main loop: 5 ms)
                const std::uint64_t c0 = pilClockUs();
                const CPilBox::Reply reply = p.poll(out);
                if (pilClockUs() - c0 > r.longestCallUs) r.longestCallUs = pilClockUs() - c0;
                if (reply == CPilBox::Reply::Answered) break;
                if (reply == CPilBox::Reply::Lost) { r.lost = true; break; }
            }
        }
        r.out = out;
    } catch (const Stuck&) {
        r.hung = true;
    }
    r.tookUs = pilClockUs() - t0;
    return r;
}
// (ms)
double ms(std::uint64_t us) { return static_cast<double>(us) / 1000.0; }
}  // namespace

int main() {
    pilstub::onTask = watchdog;
    CPilBox p("PILBOX");
    pcCommand(p, COFF);
    check(p.mode() == COFF, "the PC sent COFF: a device on the loop");

    // The PC answers at once: back from hpil() itself
    pilstub::onTask = echo;
    Result r = frame(p, 0x041);
    check(!r.hung && !r.waited && r.out == 0x041, "data frame, the PC answers at once: back from hpil()");

    // IDY in COFF: not to the PC at all
    pilstub::onTask = watchdog;
    r = frame(p, 0x600);
    check(!r.hung && !r.waited && r.out == 0x600 && pilstub::toPc.empty(), "IDY in COFF: passed on, the PC not asked");

    // The PC answers after 300 ms: hpil() doesn't wait for it, poll() gets it
    echoAt = pilClockUs() + 300000;
    pilstub::onTask = echoLate;
    r = frame(p, 0x042);
    std::printf("     answered after %.0f ms, longest call %.1f ms\n", ms(r.tookUs), ms(r.longestCallUs));
    check(!r.hung && r.waited && !r.lost && r.out == 0x042, "the PC answers after 300 ms: the answer goes on");
    check(r.longestCallUs <= 5000, "... and HIPI's main loop never stood still more than 5 ms");
    check(p.mode() == COFF, "... still COFF");

    // The PC doesn't answer (pyILPER stopped, hung, a byte lost ...)
    pilstub::onTask = watchdog;
    r = frame(p, 0x043);
    std::printf("     %s after %.0f ms, longest call %.1f ms\n", r.lost ? "lost" : "not lost", ms(r.tookUs), ms(r.longestCallUs));
    check(!r.hung, "the PC doesn't answer: HIPI doesn't hang");
    check(r.lost && r.tookUs >= 2000000 && r.tookUs < 2100000, "... the frame is dropped after 2 s");
    check(r.longestCallUs <= 5000, "... and HIPI's main loop kept running meanwhile");
    check(p.mode() == TDIS && !p.isConnected(), "... the PILBox is in TDIS");
    check(std::string(p.lostWhy()).find("2 s") != std::string::npos, "... and says why");

    // The HP-41 tries again: now without the PC's devices -- passed on
    pilstub::toPc.clear();
    r = frame(p, 0x043);
    check(!r.hung && !r.waited && r.out == 0x043 && pilstub::toPc.empty(), "the next frame: passed on unchanged (TDIS)");

    // A late answer from the PC is read away, not taken for a frame
    pilstub::fromPc = { 0x21, 0xC3 };
    for (int i = 0; i < 4; ++i) p.idle();              // (one byte each round of the main loop)
    check(pilstub::fromPc.empty() && p.mode() == TDIS, "a late answer from the PC: read away");

    // pyILPER starts again: COFF, and on as usual
    pcCommand(p, COFF);
    pilstub::onTask = echo;
    r = frame(p, 0x044);
    check(p.mode() == COFF && !r.hung && !r.waited && r.out == 0x044, "the PC sends COFF again: frames go to it again");

    // A command and its RFC: the command goes to the PC with the RFC, RFC
    // goes on once the PC has answered
    pilstub::onTask = watchdog;
    r = frame(p, 0x420);                                  // (a CMD frame: kept, passed on)
    check(r.out == 0x420 && pilstub::toPc.empty(), "a command: passed on, kept for the RFC");
    echoAt = pilClockUs() + 100000;
    pilstub::onTask = echoLate;
    r = frame(p, RFC);
    check(!r.hung && r.waited && !r.lost && r.out == RFC, "its RFC, the PC answers after 100 ms: RFC goes on");
    // ... and if the PC doesn't answer
    pilstub::onTask = watchdog;
    frame(p, 0x420);
    r = frame(p, RFC);
    check(!r.hung && r.lost && r.tookUs < 2100000 && p.mode() == TDIS, "its RFC, the PC doesn't answer: dropped after 2 s, TDIS");

    // The PC closes the port while HIPI waits: lost at once, not after 2 s
    pcCommand(p, COFI);
    closeAt = pilClockUs() + 100000;
    pilstub::onTask = closeLater;
    r = frame(p, 0x045);
    std::printf("     %s after %.0f ms\n", r.lost ? "lost" : "not lost", ms(r.tookUs));
    check(!r.hung && r.lost && r.tookUs < 200000, "the PC closes the port meanwhile: dropped at once");
    check(p.mode() == TDIS, "... TDIS");
    pilstub::connected = true;
    pilstub::onTask = watchdog;
    p.idle();                                             // (the port opens again)

    // The PC sends TDIS while HIPI waits (pyILPER shutting down)
    pcCommand(p, COFF);
    pilstub::onTask = [] {
        watchdog();
        if (pilstub::tasks == 50) { pilstub::fromPc.push_back(0x32); pilstub::fromPc.push_back(0x94); }
    };
    r = frame(p, 0x046);
    check(!r.hung && r.lost && r.tookUs < 200000 && p.mode() == TDIS, "the PC sends TDIS meanwhile: dropped at once");

    // SSRQ while waiting (COFI -> COFF) doesn't end the wait
    pilstub::onTask = watchdog;
    pcCommand(p, COFI);
    echoAt = pilClockUs() + 50000;
    pilstub::onTask = [] {
        echoLate();
        if (pilstub::tasks == 20) { pilstub::fromPc.push_back(0x32); pilstub::fromPc.push_back(0x9C); }   // SSRQ
    };
    r = frame(p, 0x047);
    check(!r.hung && !r.lost && r.out == 0x047, "SSRQ meanwhile: still waits for the answer");

    std::printf(failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
