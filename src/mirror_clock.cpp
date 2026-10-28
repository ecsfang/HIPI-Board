// mirror_clock.cpp -- see mirror_clock.h
#include "mirror_clock.h"
#include "mirror_protocol.h"
#include <cstdio>
#include <cstdlib>

namespace hipi {

namespace {

// Days since 1970-01-01 (proleptic Gregorian)
std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::string timeText(const ClockTime& t) {
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

// "3.2 s slow" / "0.4 s fast" / "" (not known)
std::string diffText(std::int32_t diffMs) {
    if (diffMs == mirror::kClkDiffUnknown) return "";
    char b[48];
    const double s = std::abs(static_cast<double>(diffMs)) / 1000.0;
    if (s >= 86400.0 * 2) std::snprintf(b, sizeof(b), "%.0f days %s", s / 86400.0, diffMs < 0 ? "slow" : "fast");
    else if (s >= 120.0) std::snprintf(b, sizeof(b), "%.0f min %s", s / 60.0, diffMs < 0 ? "slow" : "fast");
    else std::snprintf(b, sizeof(b), "%.1f s %s", s, diffMs < 0 ? "slow" : "fast");
    return b;
}

bool timeOk(const ClockTime& t) {
    return t.year >= 2000 && t.year <= 2099 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 &&
           t.hour >= 0 && t.hour <= 23 && t.minute >= 0 && t.minute <= 59 && t.second >= 0 && t.second <= 59;
}

}  // namespace

std::int64_t MirrorClock::secondsBetween(const ClockTime& a, const ClockTime& b) {
    auto secs = [](const ClockTime& t) {
        return daysFromCivil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    };
    return secs(a) - secs(b);
}

void MirrorClock::handle(std::uint8_t kind, const std::uint8_t* p, std::size_t n, std::uint64_t atUs) {
    if (kind == mirror::kClkAsk) {
        viewerKnows_ = true;
        // The second the chip is in now -- its next one is timed from here
        // (the ask was sent as a second began on the PC)
        askAt_ = atUs != 0 ? atUs : env_.nowUs();
        reportPending_ = true;
        edgeWait_ = false;
        ClockTime t;
        bool valid = false;
        if (env_.present() && env_.read(t, valid) && valid) {
            edgeFrom_ = t.second;
            edgeUntil_ = env_.nowUs() + kEdgeWaitMs * 1000ull;
            nextRead_ = env_.nowUs() + kReadEveryUs;
            edgeWait_ = true;
        }
        return;
    }
    if (kind != mirror::kClkSet || n < mirror::kClkSetLen) return;
    viewerKnows_ = true;
    ClockTime t;
    t.year = p[0] | (p[1] << 8);
    t.month = p[2]; t.day = p[3]; t.hour = p[4]; t.minute = p[5]; t.second = p[6];
    const std::int32_t diffMs = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[7]) | (static_cast<std::uint32_t>(p[8]) << 8) |
                                                          (static_cast<std::uint32_t>(p[9]) << 16) |
                                                          (static_cast<std::uint32_t>(p[10]) << 24));
    if (!timeOk(t)) return;
    if (!env_.present()) { report(0); return; }
    if (pleaseActive() || env_.mode() == ClockFromPc::Auto) {
        apply(t, diffMs, atUs != 0 ? atUs : env_.nowUs());
        return;
    }
    if (env_.mode() == ClockFromPc::Off || declined_) {
        env_.log("Clock: the PC's time not taken (Clock from PC: " +
                 std::string(declined_ ? "No was the answer" : "Off") + ")");
        report(mirror::kClkRefused);
        return;
    }
    // Ask: on the screen first; Yes asks the viewer for the time again (it
    // has moved on meanwhile)
    if (askPending_) { report(mirror::kClkAsking); return; }
    question_ = diffText(diffMs);             // "3.2 s slow" (the screen asks: set it?)
    askPending_ = true;
    report(mirror::kClkAsking);               // (the viewer waits for the answer)
}

// `t` is the second that began on the PC as the message arrived (atUs)
void MirrorClock::apply(const ClockTime& t, std::int32_t diffMs, std::uint64_t atUs) {
    const bool user = fromUser_;
    pleaseUntil_ = 0;
    fromUser_ = false;
    const std::uint32_t lead = env_.startLeadUs ? env_.startLeadUs() : 0;
    if (lead > 0 && lead < 1000000 && env_.hold && env_.release) {
        // Held now, let run so that its first tick comes as the next second
        // begins on the PC (poll())
        if (!env_.hold(t)) { applied(t, diffMs, user, false); return; }
        releasing_ = true;
        releaseAt_ = atUs + 1000000ull - lead;
        heldTime_ = t;
        heldDiffMs_ = diffMs;
        heldUser_ = user;
        return;
    }
    applied(t, diffMs, user, env_.set(t));
}

void MirrorClock::applied(const ClockTime& t, std::int32_t diffMs, bool user, bool ok) {
    if (!ok) {
        env_.log("Clock: writing the clock chip failed");
        if (user && env_.notice) env_.notice("Writing the clock failed");
        report(0);
        return;
    }
    const std::string d = diffText(diffMs);
    env_.log("Clock: set from the PC to " + timeText(t) + (d.empty() ? "" : " (it was " + d + ")"));
    if (user && env_.notice) env_.notice("Clock set from the PC: " + timeText(t));
    report(mirror::kClkWasSet);
}

void MirrorClock::report(std::uint8_t flags) {
    reportFlags_ = static_cast<std::uint8_t>(reportFlags_ | flags);
    reportPending_ = true;
}

void MirrorClock::sendReport(const ClockTime& t, bool valid, std::uint8_t flags, std::uint16_t phaseMs) {
    const bool present = env_.present();
    if (present) flags = static_cast<std::uint8_t>(flags | mirror::kClkPresent);
    if (present && valid) flags = static_cast<std::uint8_t>(flags | mirror::kClkValid);
    const char* chip = present && env_.chipName ? env_.chipName() : "";
    std::size_t cn = 0;
    while (chip[cn] != '\0' && cn < 15) ++cn;
    const std::size_t len = mirror::kClkReportLen + cn;
    std::uint8_t b[4 + mirror::kClkReportLen + 15];
    b[0] = mirror::kClock;
    b[1] = mirror::kClkReport;
    b[2] = static_cast<std::uint8_t>(len);
    b[3] = 0;
    b[4] = flags;
    b[5] = static_cast<std::uint8_t>(t.year);
    b[6] = static_cast<std::uint8_t>(t.year >> 8);
    b[7] = static_cast<std::uint8_t>(t.month);
    b[8] = static_cast<std::uint8_t>(t.day);
    b[9] = static_cast<std::uint8_t>(t.hour);
    b[10] = static_cast<std::uint8_t>(t.minute);
    b[11] = static_cast<std::uint8_t>(t.second);
    const ClockFromPc m = env_.mode();
    b[12] = m == ClockFromPc::Auto ? mirror::kClkAuto : m == ClockFromPc::Ask ? mirror::kClkAskUser : mirror::kClkOff;
    b[13] = static_cast<std::uint8_t>(phaseMs);
    b[14] = static_cast<std::uint8_t>(phaseMs >> 8);
    for (std::size_t i = 0; i < cn; ++i) b[15 + i] = static_cast<std::uint8_t>(chip[i]);
    send_(b, 4 + len);
}

void MirrorClock::poll() {
    // A held chip: let run at its moment (the last few ms waited for here)
    if (releasing_ && (env_.waitUntilUs ? env_.nowUs() + kSpinUs >= releaseAt_ : env_.nowUs() >= releaseAt_)) {
        if (env_.waitUntilUs && env_.nowUs() < releaseAt_) env_.waitUntilUs(releaseAt_);
        releasing_ = false;
        applied(heldTime_, heldDiffMs_, heldUser_, env_.release());
    }
    const std::uint64_t now = env_.nowUs();
    // Ask: the question when the screen is free
    if (askPending_ && env_.ask) {
        if (env_.ask(question_, [this](bool yes) {
                if (yes) {
                    pleaseUntil_ = env_.nowUs() + kPleaseMs * 1000ull;
                    fromUser_ = true;
                    report(mirror::kClkPlease);
                } else {
                    declined_ = true;
                    env_.log("Clock: the PC's time not taken (No)");
                    report(mirror::kClkRefused);
                }
            }))
            askPending_ = false;
    }
    // A "please" the viewer didn't answer
    if (pleaseUntil_ != 0 && now >= pleaseUntil_) {
        pleaseUntil_ = 0;
        if (fromUser_ && env_.notice) env_.notice("hipiview didn't send the time");
        fromUser_ = false;
    }
    if (!reportPending_) return;
    ClockTime t;
    bool valid = false;
    auto done = [&](bool edge) {
        // The phase: from the viewer's ask to this second's start
        std::uint16_t phase = mirror::kClkNoPhase;
        if (edge && askAt_ != 0 && now >= askAt_ && now - askAt_ < 2000000) phase = static_cast<std::uint16_t>((now - askAt_) / 1000);
        sendReport(t, valid, static_cast<std::uint8_t>(reportFlags_ | (phase != mirror::kClkNoPhase ? mirror::kClkEdge : 0)), phase);
        reportPending_ = edgeWait_ = false;
        reportFlags_ = 0;
        askAt_ = 0;
    };
    if (!edgeWait_) {
        // (no ask to time it from: at once)
        if (env_.present() && !env_.read(t, valid)) valid = false;
        done(false);
        return;
    }
    if (now < nextRead_) return;
    nextRead_ = now + kReadEveryUs;
    if (!env_.read(t, valid) || !valid) { done(false); return; }
    if (t.second != edgeFrom_) { done(true); return; }
    if (now >= edgeUntil_) done(false);       // (a clock that doesn't run)
}

void MirrorClock::viewerGone() {
    viewerKnows_ = false;
    reportPending_ = edgeWait_ = false;
    reportFlags_ = 0;
    askAt_ = 0;
    askPending_ = false;
    if (releasing_) { releasing_ = false; env_.release(); }   // (never left held)
    declined_ = false;
    pleaseUntil_ = 0;
    fromUser_ = false;
}

bool MirrorClock::setFromPc(std::string& why) {
    if (!viewerKnows_) { why = "hipiview isn't connected (or is too old to set the clock)"; return false; }
    if (!env_.present()) { why = "no clock chip"; return false; }
    pleaseUntil_ = env_.nowUs() + kPleaseMs * 1000ull;
    fromUser_ = true;
    report(mirror::kClkPlease);
    return true;
}

}  // namespace hipi
