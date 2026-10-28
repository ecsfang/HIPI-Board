// clock_test.cpp -- HIPI's clock chip set from the PC's clock: HIPI's side
// (mirror_clock.cpp) and hipiview's (clock_sync.hpp) against each other, in
// simulated time. The chip counts its seconds from when it was last set;
// the messages take a millisecond or two each way; HIPI's main loop comes
// round every millisecond, hipiview's every 16 ms.
//
//   make clock_test && ./clock_test   (in tools/hipiview)
#include "mirror_clock.h"
#include "../clock_sync.hpp"
#include "../lt7683_emu.hpp"
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// One run: a PC, a HIPI with a clock chip, the link between them
struct World {
    double t = ClockSync::civilSeconds(2026, 10, 28, 9, 15, 0) + 0.3;   // the true time (= the PC's clock)
    // The chip: its time is t + offset; set() makes it start the given second now
    bool present = true, valid = true;
    double offset = -3.2;
    int sets = 0;
    hipi::ClockFromPc mode = hipi::ClockFromPc::Auto;
    bool trusted = true;
    // Ask: the question waits here; answered by the test
    bool questionUp = false;
    std::string howOff;
    std::function<void(bool)> answer;
    bool screenBusy = false;                  // a menu is open: the question waits
    std::vector<std::string> notices, logs;
    double latencyUp = 0.002, latencyDown = 0.001;

    struct Msg { double at; std::vector<std::uint8_t> bytes; };
    std::deque<Msg> toPc, toHipi;
    std::unique_ptr<hipi::MirrorClock> hipiSide;
    std::unique_ptr<ClockSync> pcSide;
    std::vector<std::string> news;

    World() {
        hipi::ClockEnv env;
        env.present = [this] { return present; };
        env.read = [this](hipi::ClockTime& c, bool& v) {
            int y, mo, d, h, mi, s;
            ClockSync::civilFromSeconds(static_cast<long long>(std::floor(t + offset)), y, mo, d, h, mi, s);
            c.year = y; c.month = mo; c.day = d; c.hour = h; c.minute = mi; c.second = s;
            v = valid;
            return present;
        };
        env.set = [this](const hipi::ClockTime& c) {
            offset = ClockSync::civilSeconds(c.year, c.month, c.day, c.hour, c.minute, c.second) - t;
            valid = true;
            ++sets;
            return true;
        };
        env.chipName = [] { return "DS3231"; };
        env.mode = [this] { return mode; };
        env.ask = [this](const std::string& h, std::function<void(bool)> a) {
            if (screenBusy) return false;
            questionUp = true;
            howOff = h;
            answer = std::move(a);
            return true;
        };
        env.notice = [this](const std::string& s) { notices.push_back(s); };
        env.log = [this](const std::string& s) { logs.push_back(s); };
        env.nowUs = [this] { return static_cast<std::uint64_t>(t * 1e6); };
        hipiSide = std::make_unique<hipi::MirrorClock>(env, [this](const std::uint8_t* p, std::size_t n) {
            toPc.push_back({ t + latencyUp, std::vector<std::uint8_t>(p, p + n) });
        });
        ClockSync::Env penv;
        penv.localNow = [this] { return t; };
        penv.pcClockTrusted = trusted;
        pcSide = std::make_unique<ClockSync>(penv, [this](std::vector<std::uint8_t> m, double atLocal) {
            toHipi.push_back({ (atLocal < 0 ? t : atLocal) + latencyDown, std::move(m) });
        });
    }
    void untrusted() {
        ClockSync::Env penv;
        penv.localNow = [this] { return t; };
        penv.pcClockTrusted = false;
        pcSide = std::make_unique<ClockSync>(penv, [this](std::vector<std::uint8_t> m, double atLocal) {
            toHipi.push_back({ (atLocal < 0 ? t : atLocal) + latencyDown, std::move(m) });
        });
    }
    // Runs `secs`: messages delivered when due, HIPI's loop every 1 ms,
    // hipiview's every 16 ms
    void run(double secs) {
        const double end = t + secs;
        double nextPc = t;
        while (t < end) {
            t += 0.0005;
            while (!toHipi.empty() && toHipi.front().at <= t) {
                const auto m = toHipi.front().bytes;          // 'F' sid kind len(2) payload crc(2)
                toHipi.pop_front();
                const std::size_t len = static_cast<std::size_t>(m[3] | (m[4] << 8));
                hipiSide->handle(m[2], m.data() + 5, len);
            }
            if (static_cast<long long>(t * 2000) % 2 == 0) hipiSide->poll();
            while (!toPc.empty() && toPc.front().at <= t) {
                const auto m = toPc.front().bytes;            // kClock kind len(2) payload
                toPc.pop_front();
                pcSide->onPacket(m[1], m.data() + 4, m.size() - 4);
            }
            if (t >= nextPc) {
                nextPc = t + 0.016;
                pcSide->tick(t);
                const std::string s = pcSide->takeNews();
                if (!s.empty()) { news.push_back(s); std::printf("     hipiview: %s\n", s.c_str()); }
            }
        }
    }
    double chipOff() const { return offset; }   // the chip minus the true time
    bool said(const std::string& part) const {
        for (const auto& s : news) if (s.find(part) != std::string::npos) return true;
        return false;
    }
};
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        // Auto: HIPI's clock 3.2 s slow -- set when the picture is in
        World w;
        w.pcSide->sessionReady(w.t);
        w.run(5);
        std::printf("     the chip is now %+.4f s off (1 set)\n", w.chipOff());
        check(w.sets == 1 && std::fabs(w.chipOff()) < 0.01, "Auto, 3.2 s slow: set, to within 10 ms");
        check(w.said("3.2 s slow -- setting it") && w.said("set from this PC -- now"), "... hipiview says so, and how it is now");
        check(!w.logs.empty() && w.logs.back().find("it was 3.2 s slow") != std::string::npos, "... HIPI logs it");
        check(w.notices.empty(), "... no message box on HIPI (it wasn't asked for there)");
        // A day later: asked again -- right now, nothing done
        w.run(ClockSync::kRecheckSec + 3);
        check(w.sets == 1 && w.said("right"), "a day later: asked again; right -- not set again");
        // ... but if the chip has drifted meanwhile: set again
        w.offset += 0.8;
        w.run(ClockSync::kRecheckSec + 3);
        check(w.sets == 2 && std::fabs(w.chipOff()) < 0.01, "a day later, 0.8 s fast: set again");
    }
    {
        // Right already (within the tolerance): not set
        World w;
        w.offset = 0.08;
        w.pcSide->sessionReady(w.t);
        w.run(4);
        std::printf("     measured %+.4f s (true %+.4f)\n", w.pcSide->diff(), w.offset);
        check(w.sets == 0 && w.said("right"), "0.08 s fast: right, not set");
        check(std::fabs(w.pcSide->diff() - 0.08) < 0.005, "... the difference measured to a few ms (HIPI answered as its second began)");
        check(w.pcSide->chip() == "DS3231", "... the chip's name comes along");
    }
    {
        // The answer slow to come back (the picture's stream busy: 0.4 s):
        // measured all the same -- from the ask, which goes out on time
        World w;
        w.offset = 0.12;
        w.latencyUp = 0.4;
        w.pcSide->sessionReady(w.t);
        w.run(4);
        std::printf("     measured %+.4f s (true %+.4f), the answer 0.4 s on its way\n", w.pcSide->diff(), w.offset);
        check(w.sets == 0 && std::fabs(w.pcSide->diff() - 0.12) < 0.005, "the answer 0.4 s late: still measured to a few ms, not set");
    }
    {
        // The chip lost its time (not set): set
        World w;
        w.valid = false;
        w.offset = -86400.0 * 9000;
        w.pcSide->sessionReady(w.t);
        w.run(4);
        check(w.sets == 1 && std::fabs(w.chipOff()) < 0.01 && w.valid, "a clock that isn't set: set");
        check(w.said("isn't set -- setting it"), "... and said so");
    }
    {
        // Across midnight and the year: the next second is 00:00:00 on 1 January
        World w;
        w.t = ClockSync::civilSeconds(2026, 12, 31, 23, 59, 59) + 0.97;
        w.offset = 5;
        w.pcSide->sessionReady(w.t);
        w.run(5);
        int y, mo, d, h, mi, s;
        ClockSync::civilFromSeconds(static_cast<long long>(std::floor(w.t + w.offset)), y, mo, d, h, mi, s);
        check(w.sets == 1 && std::fabs(w.chipOff()) < 0.01 && y == 2027 && mo == 1 && d == 1, "set across midnight on New Year's Eve");
    }
    {
        // Off: not set by itself -- said why; Set clock from PC sets it anyway
        World w;
        w.mode = hipi::ClockFromPc::Off;
        w.pcSide->sessionReady(w.t);
        w.run(4);
        check(w.sets == 0 && w.said("Off"), "Off: not set, hipiview says Clock from PC is Off");
        std::string why;
        check(w.hipiSide->canSetFromPc(), "... Set clock from PC is offered (hipiview asked, a chip is there)");
        check(w.hipiSide->setFromPc(why), "... and chosen");
        w.run(4);
        check(w.sets == 1 && std::fabs(w.chipOff()) < 0.01, "... then it's set");
        check(w.notices.size() == 1 && w.notices[0].find("Clock set from the PC") != std::string::npos,
              "... and a message box on HIPI says so");
    }
    {
        // Ask: the question on HIPI's screen (after a menu closes); Yes sets it
        World w;
        w.mode = hipi::ClockFromPc::Ask;
        w.screenBusy = true;
        w.pcSide->sessionReady(w.t);
        w.run(3);
        check(!w.questionUp && w.sets == 0, "Ask: a menu is open -- the question waits");
        w.screenBusy = false;
        w.run(0.1);
        check(w.questionUp && w.howOff == "3.2 s slow", "... then asked: \"3.2 s slow\"");
        check(w.said("asks on its screen"), "... hipiview knows the user is asked (and doesn't keep sending)");
        w.run(5);                                  // (the user takes a while)
        if (!w.answer) return 1;
        w.questionUp = false;
        w.answer(true);
        w.run(3);
        check(w.sets == 1 && std::fabs(w.chipOff()) < 0.01, "... Yes: the time fetched afresh and set");
        check(w.notices.size() == 1, "... said on HIPI's screen");
    }
    {
        // Ask, No: not set, not asked again in this session
        World w;
        w.mode = hipi::ClockFromPc::Ask;
        w.pcSide->sessionReady(w.t);
        w.run(3);
        check(w.questionUp && w.answer != nullptr, "Ask: asked");
        if (!w.answer) return 1;
        w.questionUp = false;
        w.answer(false);
        w.run(2);
        check(w.sets == 0 && w.said("didn't take"), "Ask, No: not set");
        w.pcSide->tick(w.t + ClockSync::kRecheckSec + 1);    // (a day later)
        w.run(3);
        check(!w.questionUp && w.sets == 0, "... and not asked again in this session");
    }
    {
        // The PC's clock not synchronised: not set by itself; asked for on HIPI: set
        World w;
        w.untrusted();
        w.pcSide->sessionReady(w.t);
        w.run(4);
        check(w.sets == 0 && w.said("isn't synchronised"), "PC's clock not synchronised: not set, said why");
        std::string why;
        w.hipiSide->setFromPc(why);
        w.run(4);
        check(w.sets == 1, "... Set clock from PC on HIPI: set all the same");
    }
    {
        // No clock chip
        World w;
        w.present = false;
        w.pcSide->sessionReady(w.t);
        w.run(3);
        std::string why;
        check(w.said("no clock chip") && !w.hipiSide->canSetFromPc() && !w.hipiSide->setFromPc(why),
              "no clock chip: said, and Set clock from PC not offered");
    }
    {
        // An older hipiview never asks: HIPI never sends a report (it would
        // be a stream error there), and offers no Set clock from PC
        World w;
        w.run(5);
        std::string why;
        check(w.toPc.empty() && w.pcSide->reports() == 0, "a viewer that doesn't ask: no report sent to it");
        check(!w.hipiSide->canSetFromPc() && !w.hipiSide->setFromPc(why) && !why.empty(),
              "... and Set clock from PC isn't offered");
    }
    {
        // Lost messages: the ask, then the time -- asked again, set all the same
        World w;
        w.pcSide->sessionReady(w.t);
        w.toHipi.clear();                          // (the ask lost)
        w.run(7);
        check(w.sets == 1, "the ask lost: asked again, set");
        w.offset = -2;
        w.pcSide->sessionReady(w.t);
        w.run(0.5);                                // (the report back; the time on its way)
        w.toHipi.clear();                          // (the time lost)
        w.run(6);
        check(w.sets == 2 && std::fabs(w.chipOff()) < 0.01, "the time lost: asked again, set");
    }
    {
        // The session ends: HIPI forgets the viewer
        World w;
        w.pcSide->sessionReady(w.t);
        w.run(3);
        w.hipiSide->viewerGone();
        check(!w.hipiSide->canSetFromPc(), "viewer gone: Set clock from PC no longer offered");
    }
    {
        // hipiview's emulator takes the kClock packet out of the stream (no
        // stream error), with a mark where a session began
        LT7683Emu emu;
        std::vector<std::uint8_t> s = { hipi::mirror::kHello, 'H', 'I', 'P', 'I', 'M', 'I', 'R', hipi::mirror::kVersion,
                                        hipi::mirror::kChipLT7683, 0x00, 0x04, 0x58, 0x02 };
        const std::uint8_t rep[] = { hipi::mirror::kClock, hipi::mirror::kClkReport, 17, 0,
                                     hipi::mirror::kClkPresent | hipi::mirror::kClkValid, 0xEA, 0x07, 10, 28, 9, 15, 3,
                                     hipi::mirror::kClkAuto, 0xFF, 0xFF, 'D', 'S', '3', '2', '3', '1' };
        s.insert(s.end(), rep, rep + sizeof rep);
        emu.feed(s.data(), s.size());
        const auto r = emu.takeClockReports();
        check(emu.streamErrors() == 0 && r.size() == 2 && r[0].kind == 0 && r[1].kind == hipi::mirror::kClkReport &&
              r[1].data.size() == 17, "the emulator: the report taken out of the stream, no error");
    }
    std::printf(failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
