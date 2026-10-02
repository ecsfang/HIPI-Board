#define MODULE "BKLT"
#include "backlight.h"
#include "Screen.hpp"
#include "usb_serial.h"
#include "pico/time.h"

namespace hipi {

namespace {
Screen* screen_ = nullptr;
bool dimmed_ = false;
absolute_time_t dimAt_ = nil_time;
}

void backlight_init(Screen* screen) {
    screen_ = screen;
    dimmed_ = false;
    dimAt_ = make_timeout_time_ms(kDimAfterMs);
}

void backlight_activity() {
    if (screen_ == nullptr) return;
    dimAt_ = make_timeout_time_ms(kDimAfterMs);
    if (dimmed_) {
        dimmed_ = false;
        // The brightness chosen in the menu (Screen keeps it; dimming only
        // sets the hardware, not Screen's value)
        screen_->display()->brightness(screen_->brightness());
        MLOGF("backlight restored");
    }
}

bool backlight_wakeByTouch() {
    const bool wasDimmed = dimmed_;
    backlight_activity();
    return wasDimmed;
}

void backlight_poll() {
    if (screen_ == nullptr || dimmed_ || !time_reached(dimAt_)) return;
    // Never brighter than what's set (if that's already below kDimLevel)
    const std::uint8_t level = screen_->brightness() < kDimLevel ? screen_->brightness() : kDimLevel;
    screen_->display()->brightness(level);
    dimmed_ = true;
    MLOGF("no activity for %lu min -- backlight dimmed", static_cast<unsigned long>(kDimAfterMs / 60000));
}

bool backlight_dimmed() { return dimmed_; }

}  // namespace hipi
