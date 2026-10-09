// input.hpp -- hipiview working HIPI's touch screen and buttons
// (Mirror to PC < Control >, see mirror_protocol.h, INPUT):
//   RemoteInput  turns the mouse and keys into 'T' / 'B' messages for HIPI,
//                with the minimum press time and the repeats while held
//   TouchMarks   draws a ring where the mouse presses, in the window and in
//                the video, so a viewer can see where was touched
#pragma once
#include "../../include/mirror_protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

// ── Messages to HIPI ────────────────────────────────────────────────────
// send(msg): a message with its session byte (msg[1]) still to be filled
// in -- the serial reader does that, it knows the session.
class RemoteInput {
public:
    using Send = std::function<void(std::vector<std::uint8_t>)>;
    explicit RemoteInput(Send send) : send_(std::move(send)) {}

    // Button codes (mirror_protocol.h)
    enum : std::uint8_t { kShift = 1, kOk = 2, kUp = 3, kDown = 4, kX = 5, kNextView = 6, kPrevView = 7 };

    // The mouse, in panel pixels
    void touchDown(int x, int y, double now) {
        if (upAt_ >= 0) flushUp();                // a quick click before: lift it first
        down_ = true;
        x_ = x; y_ = y;
        downAt_ = now;
        touch(1, now);
    }
    void touchMove(int x, int y, double now) {
        if (!down_) return;
        x_ = x; y_ = y;
        moved_ = true;
        if (now - sentAt_ >= kMoveEvery) touch(1, now);
    }
    void touchUp(double now) {
        if (!down_) return;
        down_ = false;
        if (moved_) touch(1, now);                // where it ended
        // A click shorter than HIPI's debouncing would be missed: hold it
        if (now - downAt_ < kMinHold) upAt_ = downAt_ + kMinHold;
        else touch(0, now);
    }

    // A button: pressed / released
    void button(std::uint8_t code, bool pressed, double now) {
        if (pressed) {
            if (code >= kNextView) { buttonMsg(code, 1); buttonMsg(code, 0); return; }   // a one-off
            held_ = code;
            heldSentAt_ = now;
            buttonMsg(code, 1);
        } else {
            if (held_ == code) held_ = 0;
            buttonMsg(code, 0);
        }
    }
    // A short press (mouse wheel, Shift before a key)
    void tap(std::uint8_t code) { buttonMsg(code, 1); buttonMsg(code, 0); }

    // Call every frame: the delayed lift, moves, and the repeats while held
    void tick(double now) {
        if (upAt_ >= 0 && now >= upAt_) flushUp();
        if (down_ && moved_ && now - sentAt_ >= kMoveEvery) touch(1, now);
        if (down_ && now - sentAt_ >= kRepeat) touch(1, now);
        if (held_ && now - heldSentAt_ >= kRepeat) { heldSentAt_ = now; buttonMsg(held_, 1); }
    }

    bool touching() const { return down_; }

private:
    static constexpr double kMinHold = 0.10;     // s, longer than HIPI's 60 ms debounce
    static constexpr double kRepeat = 0.20;      // s, "still down" (HIPI gives up after 1 s)
    static constexpr double kMoveEvery = 0.02;   // s, at most 50 moves a second

    void touch(std::uint8_t state, double now) {
        namespace mp = hipi::mirror;
        const auto x = static_cast<std::uint16_t>(x_), y = static_cast<std::uint16_t>(y_);
        send_({ mp::kMsgTouch, 0, static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8),
                static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8), state });
        sentAt_ = now;
        moved_ = false;
    }
    void flushUp() {
        upAt_ = -1;
        if (!down_) touch(0, 0);
    }
    void buttonMsg(std::uint8_t code, std::uint8_t state) {
        send_({ hipi::mirror::kMsgButton, 0, code, state });
    }

    Send send_;
    bool down_ = false, moved_ = false;
    int x_ = 0, y_ = 0;
    double downAt_ = 0, sentAt_ = 0, upAt_ = -1;
    std::uint8_t held_ = 0;
    double heldSentAt_ = 0;
};

// ── The touch ring ──────────────────────────────────────────────────────
class TouchMarks {
public:
    void down(int x, int y) { on_ = true; x_ = x; y_ = y; fading_ = false; }
    void move(int x, int y) { if (on_) { x_ = x; y_ = y; } }
    void up(double now) { if (on_) { on_ = false; fading_ = true; upAt_ = now; } }

    // A touch as saved in a log ('T' x(2) y(2) state)
    void apply(const std::uint8_t* p, std::size_t n, double now) {
        if (n < 6 || p[0] != hipi::mirror::kMsgTouch) return;
        const int x = p[1] | (p[2] << 8), y = p[3] | (p[4] << 8);
        if (p[5]) { if (on_) move(x, y); else down(x, y); }
        else up(now);
    }

    // Draws into the picture (0xAARRGGBB)
    void draw(std::uint32_t* px, int w, int h, double now) const {
        double a = 1.0;
        if (!on_) {
            if (!fading_) return;
            a = 1.0 - (now - upAt_) / kFade;
            if (a <= 0) return;
        }
        const int r = kRadius, x0 = std::max(0, x_ - r - 3), x1 = std::min(w - 1, x_ + r + 3);
        const int y0 = std::max(0, y_ - r - 3), y1 = std::min(h - 1, y_ + r + 3);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double d = std::hypot(x - x_, y - y_);
                double ring = 1.0 - std::fabs(d - r) / 2.5;           // a 5 px wide ring
                double shade = (d >= r + 1.5 && d < r + 3.5) ? 0.5 : 0.0;   // dark edge outside
                double fill = (on_ && d < r - 2) ? 0.25 : 0.0;       // light inside while pressed
                ring = std::max(0.0, ring);
                if (ring <= 0 && shade <= 0 && fill <= 0) continue;
                std::uint32_t& p = px[static_cast<std::size_t>(y) * w + x];
                if (shade > 0) p = blend(p, 0x000000, shade * a);
                if (fill > 0) p = blend(p, 0xFFFFFF, fill * a);
                if (ring > 0) p = blend(p, 0xFFFFFF, std::min(1.0, ring) * 0.85 * a);
            }
        }
    }

private:
    static constexpr int kRadius = 22;
    static constexpr double kFade = 0.4;          // s after lifting

    static std::uint32_t blend(std::uint32_t p, std::uint32_t c, double a) {
        auto ch = [&](int s) {
            const double v = ((p >> s) & 0xFF) * (1 - a) + ((c >> s) & 0xFF) * a;
            return static_cast<std::uint32_t>(std::lround(v)) << s;
        };
        return 0xFF000000u | ch(16) | ch(8) | ch(0);
    }

    bool on_ = false, fading_ = false;
    int x_ = 0, y_ = 0;
    double upAt_ = 0;
};
