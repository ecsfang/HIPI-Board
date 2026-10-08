#define MODULE "BKLT"
#include "backlight.h"
#include "Screen.hpp"
#include "config.hpp"
#include "clock.h"
#include "saver_anim.h"
#include "pico/rand.h"
#include "plotterview.h"
#include "boardui.h"
#include "usb_serial.h"
#include "pico/time.h"

extern hipi::Config config;

namespace hipi {

namespace {
Screen* screen_ = nullptr;
enum class Saver { None, Dimmed, Clock };
Saver saver_ = Saver::None;
absolute_time_t dimAt_ = nil_time;
bool showClockNow_ = false;      // backlight_showClockNow(): the clock, whatever the mode

// Fading the backlight down while the clock is shown (kClockFadeMs)
bool fading_ = false;
std::uint8_t fadeFrom_ = 0, fadeTo_ = 0;
absolute_time_t fadeStart_ = nil_time;
absolute_time_t nextFadeStep_ = nil_time;
constexpr std::uint32_t kFadeStepMs = 50;

std::uint8_t dimLevel() {
    // Never brighter than what's set (if that's already below kDimLevel)
    return screen_->brightness() < kDimLevel ? screen_->brightness() : kDimLevel;
}

void fadeStep() {
    if (!fading_ || !time_reached(nextFadeStep_)) return;
    nextFadeStep_ = make_timeout_time_ms(kFadeStepMs);
    const std::int64_t elapsedMs = absolute_time_diff_us(fadeStart_, get_absolute_time()) / 1000;
    std::uint8_t level = fadeTo_;
    if (elapsedMs < static_cast<std::int64_t>(kClockFadeMs)) {
        level = static_cast<std::uint8_t>(fadeFrom_ - (fadeFrom_ - fadeTo_) * elapsedMs /
                                                      static_cast<std::int64_t>(kClockFadeMs));
    } else {
        fading_ = false;
    }
    screen_->display()->brightness(level);
}

void startClockDim() {
    fadeFrom_ = screen_->brightness();
    fadeTo_ = dimLevel();
    if (kClockFadeMs == 0 || fadeFrom_ <= fadeTo_) {
        screen_->display()->brightness(fadeTo_);      // at once
        fading_ = false;
        return;
    }
    fadeStart_ = get_absolute_time();
    nextFadeStep_ = nil_time;
    fading_ = true;
}
}

void backlight_init(Screen* screen) {
    screen_ = screen;
    saver_ = Saver::None;
    dimAt_ = make_timeout_time_ms(kDimAfterMs);
}

void backlight_activity() {
    if (screen_ == nullptr) return;
    dimAt_ = make_timeout_time_ms(kDimAfterMs);
    if (saver_ == Saver::Dimmed) {
        // The brightness chosen in the menu (Screen keeps it; dimming only
        // sets the hardware, not Screen's value)
        screen_->display()->brightness(screen_->brightness());
        MLOGF("backlight restored");
    } else if (saver_ == Saver::Clock) {
        fading_ = false;
        screen_->display()->brightness(screen_->brightness());   // full brightness at once
        plotterview_leaveClock();             // back to the view before
        boardui_saverEnd();                   // menu, strip... as they were
        MLOGF("clock screen saver ended");
    }
    saver_ = Saver::None;
}

bool backlight_wakeByTouch() {
    const bool wasSaving = saver_ != Saver::None;
    backlight_activity();
    return wasSaving;
}

void backlight_poll() {
    if (screen_ == nullptr) return;
    fadeStep();
    if (saver_ != Saver::None || !time_reached(dimAt_)) return;
    const bool manual = showClockNow_;
    showClockNow_ = false;
    std::uint8_t mode = config.saverMode();
    // "Show now" with Dim/Off selected: show the clock
    if (manual && (mode == Config::SaverDim || mode == Config::SaverOff)) mode = Config::SaverClock;
    // Clock without a usable clock (no RTC, time not set): the fallback
    if (mode == Config::SaverClock && !clock_available()) mode = config.clockFallback();
    if (mode == Config::SaverOff) {
        dimAt_ = make_timeout_time_ms(kDimAfterMs);   // nothing to do; look again later
        return;
    }
    bool lcd = config.clockStyle() == 1;
    if (mode == Config::SaverRandom) {
        // One of the animations (saver_anim.h: kRandomAnims), in a random style
        constexpr int n = static_cast<int>(sizeof(kRandomAnims) / sizeof(kRandomAnims[0]));
        const AnimKind k = kRandomAnims[get_rand_32() % n];
        mode = k == AnimKind::Rain ? Config::SaverRain : Config::SaverGoose;
        lcd = (get_rand_32() & 1) != 0;
    }
    if (mode == Config::SaverClock || mode == Config::SaverRain || mode == Config::SaverGoose) {
        clock_setShow(mode == Config::SaverRain  ? SaverShow::Rain
                    : mode == Config::SaverGoose ? SaverShow::Goose : SaverShow::Clock, lcd);
        boardui_saverBegin();                 // the saver gets the whole panel
        plotterview_showClock();              // the screen saver view
        saver_ = Saver::Clock;
        startClockDim();                      // dimmed too
        if (manual) MLOGF("screen saver shown (Show now)");
        else        MLOGF("no activity for %lu min -- screen saver", static_cast<unsigned long>(kDimAfterMs / 60000));
        return;
    }
    // Dim
    screen_->display()->brightness(dimLevel());
    saver_ = Saver::Dimmed;
    MLOGF("no activity for %lu min -- backlight dimmed", static_cast<unsigned long>(kDimAfterMs / 60000));
}

bool backlight_dimmed() { return saver_ != Saver::None; }

void backlight_showClockNow() {
    // A moment later, so lifting the finger from the menu doesn't end it
    showClockNow_ = true;
    dimAt_ = make_timeout_time_ms(1500);
}

}  // namespace hipi
