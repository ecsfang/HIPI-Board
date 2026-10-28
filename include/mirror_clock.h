// mirror_clock.h -- HIPI's side of setting its clock chip from the PC, over
// the display mirror (mirror_protocol.h, CLOCK). hipiview asks how HIPI's
// clock is (kClkAsk); HIPI answers (kClkReport) as a second begins on the
// chip, so the viewer can measure the difference to a few ms. If it's off
// (or not set at all), the viewer sends the PC's time (kClkSet) as a second
// begins on the PC, and HIPI writes it to the chip -- which also restarts the
// chip's count of that second, so the clock is then right to a few ms.
//
// Clock from PC (Settings -> Screen saver): Auto -- set it whenever the
// viewer says so; Ask -- ask on the screen first; Off -- never by itself.
// "Set clock from PC" (that menu, and the Hardware test page) sets it in
// any case.
//
// No Pico SDK here: the clock chip, the settings and the screen come in
// through ClockEnv (pico_main.cpp), so the PC tests run it as it is.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace hipi {

struct ClockTime {
    int year = 2000, month = 1, day = 1;   // 2000..2099
    int hour = 0, minute = 0, second = 0;  // 24 hours
};

enum class ClockFromPc : std::uint8_t { Off = 0, Auto = 1, Ask = 2 };

struct ClockEnv {
    std::function<bool()> present;                             // a clock chip answers
    std::function<bool(ClockTime& t, bool& valid)> read;      // false: couldn't be read
    std::function<bool(const ClockTime& t)> set;
    std::function<const char*()> chipName;                    // "DS3231"
    std::function<ClockFromPc()> mode;                        // the setting
    // Ask: "set the clock from the PC?" on the screen (Yes / No); howOff:
    // "3.2 s slow" ("" not known); false = not now (a menu is open ...) --
    // it's asked again a little later
    std::function<bool(const std::string& howOff, std::function<void(bool yes)> answer)> ask;
    // A set the user asked for (the menus, Ask): said on the screen
    std::function<void(const std::string& text)> notice;
    std::function<void(const std::string& line)> log;
    std::function<std::uint64_t()> nowUs;                     // a microsecond clock
};

class MirrorClock {
public:
    // A packet into the stream (kClock kind len payload)
    using Send = std::function<void(const std::uint8_t* p, std::size_t n)>;
    MirrorClock(ClockEnv env, Send send) : env_(std::move(env)), send_(std::move(send)) {}

    // A message from the viewer ('F' kClkAsk / kClkSet); atUs: when it
    // arrived (nowUs()), 0 = now
    void handle(std::uint8_t kind, const std::uint8_t* p, std::size_t n, std::uint64_t atUs = 0);
    // From the main loop: the answer as a second begins, the question (Ask)
    void poll();
    // The viewer's session ended
    void viewerGone();

    // "Set clock from PC" can be offered: a viewer that knows CLOCK (it
    // asked in this session) and a clock chip
    bool canSetFromPc() const { return viewerKnows_ && env_.present && env_.present(); }
    // ... and done: the viewer is asked for the time; false + why if it can't
    bool setFromPc(std::string& why);

    // The difference between two times in seconds (a - b)
    static std::int64_t secondsBetween(const ClockTime& a, const ClockTime& b);

    static constexpr std::uint32_t kPleaseMs = 10000;   // a "please" waits this long for the time
    static constexpr std::uint32_t kEdgeWaitMs = 1500;  // the next second, at most this long
    static constexpr std::uint32_t kReadEveryUs = 2000; // the chip read this often meanwhile

private:
    void report(std::uint8_t flags);          // as the next second begins (poll())
    void sendReport(const ClockTime& t, bool valid, std::uint8_t flags, std::uint16_t phaseMs);
    void apply(const ClockTime& t, std::int32_t diffMs);
    bool pleaseActive() const { return pleaseUntil_ != 0 && env_.nowUs() < pleaseUntil_; }

    ClockEnv env_;
    Send send_;
    bool viewerKnows_ = false;                 // the viewer asked (kClkAsk) in this session
    // A report waiting for the next second (flags to send with it)
    bool reportPending_ = false, edgeWait_ = false;
    std::uint8_t reportFlags_ = 0;
    int edgeFrom_ = -1;                        // the second it started in
    std::uint64_t askAt_ = 0;                  // when the viewer's kClkAsk arrived (0: none)
    std::uint64_t edgeUntil_ = 0, nextRead_ = 0;
    // The user asked to set it (menu, Ask's Yes): the viewer's time is taken
    // until then, whatever the setting
    std::uint64_t pleaseUntil_ = 0;
    bool fromUser_ = false;                    // (the set is said on the screen)
    // Ask: a question waiting for the screen, and what to set if Yes
    bool askPending_ = false;
    std::string question_;
    bool declined_ = false;                    // No: not asked again in this session
};

}  // namespace hipi
