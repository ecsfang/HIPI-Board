// clock_sync.hpp -- hipiview's side of setting HIPI's clock chip from this
// PC's clock (mirror_protocol.h, CLOCK; HIPI's side: mirror_clock.h).
//
// When the picture is in (each session) -- and once a day after that --
// HIPI is asked how its clock is (kClkAsk), the question sent as a second
// begins on the PC. HIPI notes when it arrived and how long after that its
// chip's next second began, and answers (kClkReport) with that: the
// difference to the PC's clock to a few ms -- however long the answer takes
// to come back through the picture's stream. If HIPI's clock is more than kTolerance off, or not set
// at all, or the user asked on HIPI (kClkPlease), the PC's time is sent
// (kClkSet) to arrive as the next second begins on the PC: HIPI writes it to
// the chip, which restarts the chip's count of that second. HIPI decides
// whether to take it (its setting Clock from PC: Auto / Ask / Off) and
// answers again (kClkWasSet / kClkRefused), with its clock as it is now.
//
// The PC's clock is local time (the chip has no time zone): summer and
// winter time come along by themselves. If the PC's clock isn't kept right
// (pcClockTrusted false: not synchronised with the internet) it's only
// said, not sent -- unless the user asked on HIPI.
//
// No SDL, no serial port here (clock_test runs it): the time comes from
// Env, messages go out through Send, timed to the second.
#pragma once
#include "../../include/mirror_protocol.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

class ClockSync {
public:
    // A message for HIPI, to go out when the PC's clock reaches atLocal
    // (local time in seconds, as Env::localNow; < 0: at once)
    using Send = std::function<void(std::vector<std::uint8_t> msg, double atLocal)>;
    struct Env {
        // The PC's local time, in seconds since 1970-01-01 as if it were UTC
        // (so whole seconds are the local clock's seconds), fractions included
        std::function<double()> localNow;
        bool pcClockTrusted = true;
    };
    static constexpr double kTolerance = 0.25;       // seconds off before HIPI's clock is set
    static constexpr double kRecheckSec = 24 * 3600; // asked again this often
    static constexpr double kLeadSec = 0.05;         // a second closer than this: the one after
    static constexpr double kAnswerSec = 3.0;        // no answer this long: asked again
    static constexpr int kTries = 5;                 // ... this often at most
    static constexpr double kOneWay = 0.001;         // a message to HIPI takes about this long
    static constexpr int kSetsPerSession = 3;        // set again if still off after setting, at most

    ClockSync(Env env, Send send) : env_(std::move(env)), send_(std::move(send)) {}

    // The picture is in (a new session): ask HIPI about its clock
    void sessionReady(double now) {
        now_ = now;
        asked_ = true;
        lastAsk_ = now;
        tries_ = 0;
        sets_ = 0;
        verify_ = false;
        ask(now);
    }
    // The session ended (the picture comes again)
    void sessionLost() { asked_ = false; waiting_ = false; }
    bool askedThisSession() const { return asked_; }

    // Now and then: ask again once a day, and when an answer didn't come
    // (a message lost on the way)
    void tick(double now) {
        now_ = now;
        if (asked_ && now - lastAsk_ > kRecheckSec) sessionReady(now);
        if (waiting_ && now - waitFrom_ > kAnswerSec) {
            if (tries_ < kTries) { ++tries_; ask(now); }
            else waiting_ = false;
        }
    }

    // A kClock packet from HIPI; pcAt: the PC's local time when it arrived
    // (< 0: now)
    void onPacket(std::uint8_t kind, const std::uint8_t* p, std::size_t n, double pcAt = -1) {
        using namespace hipi::mirror;
        if (kind != kClkReport || n < kClkReportLen) return;
        const double pcNow = pcAt >= 0 ? pcAt : env_.localNow();
        const std::uint8_t flags = p[0];
        waiting_ = false;
        mode_ = p[8];
        chip_.assign(reinterpret_cast<const char*>(p + kClkReportLen), n - kClkReportLen);
        ++reports_;
        if (!(flags & kClkPresent)) { say("HIPI has no clock chip"); return; }
        const bool valid = (flags & kClkValid) != 0;
        hipiSec_ = civilSeconds(p[1] | (p[2] << 8), p[3], p[4], p[5], p[6], p[7]);
        const std::uint16_t phase = static_cast<std::uint16_t>(p[9] | (p[10] << 8));
        // HIPI's clock minus the PC's. Timed from our ask: the chip's second
        // hipiSec began `phase` ms after the ask arrived, which was when the
        // PC's second askAt_ began (+ the way there). Else from the middle of
        // HIPI's second, as it arrived here (+-0.5 s, and the stream's delay)
        exact_ = (flags & kClkEdge) && phase != kClkNoPhase && askAt_ > 0;
        haveDiff_ = valid;
        diff_ = !valid ? 0.0
              : exact_ ? hipiSec_ - phase / 1000.0 - (askAt_ + kOneWay)
                       : hipiSec_ + 0.5 - pcNow;
        askAt_ = 0;
        if (flags & kClkWasSet) {
            // Set: measured once more, to say how right it is now
            verify_ = true;
            say("HIPI's clock set from this PC -- checking it");
            ask(now_);
            return;
        }
        if (verify_ && !(flags & kClkPlease)) {
            verify_ = false;
            if (!(valid && std::fabs(diff_) > kTolerance && sets_ < kSetsPerSession)) {
                say(valid ? "HIPI's clock set from this PC -- now " + offText() : std::string("HIPI's clock set, but it reads as not set"));
                return;
            }
        }
        if (flags & kClkAsking) { say("HIPI asks on its screen whether to take this PC's time"); return; }
        if (flags & kClkRefused) { say("HIPI didn't take this PC's time (" + modeText() + ")" + (valid ? " -- " + offText() : std::string())); return; }
        const bool please = (flags & kClkPlease) != 0;
        const bool off = !valid || std::fabs(diff_) > kTolerance;
        if (please || (off && mode_ != kClkOff && env_.pcClockTrusted)) {
            ++sets_;
            setNow(env_.localNow());          // (from now: the next second to come)
            say(please ? "HIPI asks for the time -- sending it"
                       : (valid ? "HIPI's clock is " + offText() + " -- setting it" : "HIPI's clock isn't set -- setting it"));
            return;
        }
        if (!off) say("HIPI's clock is right (" + offText() + ")");
        else if (mode_ == kClkOff) say("HIPI's clock is " + (valid ? offText() : std::string("not set")) + " (Clock from PC is Off on HIPI)");
        else say("HIPI's clock is " + (valid ? offText() : std::string("not set")) + " -- not set: this PC's clock isn't synchronised");
    }

    // What's new to say ("" nothing): the title bar and the log
    std::string takeNews() { std::string s; s.swap(news_); return s; }
    const std::string& status() const { return status_; }
    int reports() const { return reports_; }
    bool haveDiff() const { return haveDiff_; }
    double diff() const { return diff_; }              // HIPI minus the PC, seconds
    const std::string& chip() const { return chip_; }

    // Local time <-> seconds (days from civil, as if UTC)
    static double civilSeconds(int y, int mo, int d, int h, int mi, int s) {
        y -= mo <= 2;
        const long long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153u * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
        return static_cast<double>(days * 86400 + h * 3600 + mi * 60 + s);
    }
    static void civilFromSeconds(long long t, int& y, int& mo, int& d, int& h, int& mi, int& s) {
        long long z = t >= 0 ? t / 86400 : (t - 86399) / 86400;
        const long long sod = t - z * 86400;
        h = static_cast<int>(sod / 3600); mi = static_cast<int>(sod / 60 % 60); s = static_cast<int>(sod % 60);
        z += 719468;
        const long long era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        y = static_cast<int>(static_cast<long long>(yoe) + era * 400 + (mo <= 2));
    }

    // An 'F' message (the session byte: the serial reader)
    static std::vector<std::uint8_t> message(std::uint8_t kind, const std::uint8_t* p, std::size_t n) {
        std::vector<std::uint8_t> m = { hipi::mirror::kMsgFile, 0, kind, static_cast<std::uint8_t>(n),
                                        static_cast<std::uint8_t>(n >> 8) };
        m.insert(m.end(), p, p + n);
        const std::uint16_t crc = hipi::mirror::crc16(m.data() + 2, 3 + n);
        m.push_back(static_cast<std::uint8_t>(crc));
        m.push_back(static_cast<std::uint8_t>(crc >> 8));
        return m;
    }

private:
    // Asked as the next second begins on the PC (HIPI times its chip from it)
    void ask(double now) {
        waiting_ = true;
        waitFrom_ = now;
        const double pc = env_.localNow();
        askAt_ = std::floor(pc) + 1.0;
        if (askAt_ - pc < kLeadSec) askAt_ += 1.0;
        send_(message(hipi::mirror::kClkAsk, nullptr, 0), askAt_);
    }
    // The PC's time for HIPI, timed to arrive as the next second begins
    // (HIPI answers kClkWasSet / kClkRefused / kClkAsking -- or, if it got
    // lost, it's asked again)
    void setNow(double pcNow) {
        waiting_ = true;
        waitFrom_ = now_ + (std::floor(pcNow) + 2.0 - pcNow);
        double at = std::floor(pcNow) + 1.0;
        if (at - pcNow < kLeadSec) at += 1.0;
        int y, mo, d, h, mi, s;
        civilFromSeconds(static_cast<long long>(at), y, mo, d, h, mi, s);
        const std::int32_t diffMs = haveDiff_ ? static_cast<std::int32_t>(std::lround(std::max(-2e9 / 1000, std::min(2e9 / 1000, diff_)) * 1000))
                                              : hipi::mirror::kClkDiffUnknown;
        const std::uint8_t p[hipi::mirror::kClkSetLen] = {
            static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8), static_cast<std::uint8_t>(mo),
            static_cast<std::uint8_t>(d), static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(mi),
            static_cast<std::uint8_t>(s),
            static_cast<std::uint8_t>(diffMs), static_cast<std::uint8_t>(diffMs >> 8),
            static_cast<std::uint8_t>(diffMs >> 16), static_cast<std::uint8_t>(diffMs >> 24) };
        send_(message(hipi::mirror::kClkSet, p, sizeof p), at);
    }
    // "3.2 s slow", "+0.012 s" ...
    std::string offText() const {
        char b[64];
        const double a = std::fabs(diff_);
        if (a >= 2 * 86400) std::snprintf(b, sizeof b, "%.0f days %s", a / 86400, diff_ < 0 ? "slow" : "fast");
        else if (a >= 120) std::snprintf(b, sizeof b, "%.0f min %s", a / 60, diff_ < 0 ? "slow" : "fast");
        else if (a > kTolerance) std::snprintf(b, sizeof b, "%.1f s %s", a, diff_ < 0 ? "slow" : "fast");
        else std::snprintf(b, sizeof b, exact_ ? "%+.3f s" : "within a second, %+.1f s", diff_);
        return b;
    }
    std::string modeText() const {
        using namespace hipi::mirror;
        return mode_ == kClkOff ? "Clock from PC is Off on HIPI" : mode_ == kClkAskUser ? "No on HIPI" : "HIPI refused";
    }
    void say(const std::string& s) { status_ = s; news_ = s; }

    Env env_;
    Send send_;
    bool asked_ = false;
    double lastAsk_ = 0;
    bool waiting_ = false;                    // an answer is due (asked, or the time sent)
    double askAt_ = 0;                        // the PC's second our ask was sent at (0: none)
    bool verify_ = false;                     // measuring after a set
    int sets_ = 0;                            // sets in this session
    double waitFrom_ = 0, now_ = 0;
    int tries_ = 0;
    int reports_ = 0;
    std::uint8_t mode_ = hipi::mirror::kClkAuto;
    std::string chip_, status_, news_;
    bool haveDiff_ = false, exact_ = false;
    double diff_ = 0, hipiSec_ = 0;
};
