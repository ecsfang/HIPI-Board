// deframer.hpp -- the viewer's end of the display mirror's reliable link
// (mirror_protocol.h, FRAMING): unpacks the numbered frames, checks their
// CRC, passes on the payload in order, acknowledges what arrived ('K'),
// and asks for a resend when a frame is missing ('N'). HIPI keeps every
// frame until it is acknowledged, so a lost USB packet costs a resend,
// not the picture.
//
// Used live (with a send function) and for replaying a saved raw stream
// (without one -- the resends are in the saved stream already).
#pragma once
#include "../../include/mirror_protocol.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class Link {
public:
    using Send = std::function<void(const std::uint8_t*, std::size_t)>;

    // A new session was asked for: wait for its first frame
    void reset() { sid_ = -1; expected_ = 0; gap_ = false; acked_ = 0; heartbeat_ = false; reack_ = false; buf_.clear(); }

    // Raw bytes in; out(payload, n) for every frame, in order, once
    template <typename F>
    void feed(const std::uint8_t* p, std::size_t n, double now, F&& out, const Send& send) {
        namespace mp = hipi::mirror;
        buf_.insert(buf_.end(), p, p + n);
        std::size_t pos = 0;
        while (buf_.size() - pos >= mp::kFrameHeader) {
            const std::uint8_t* f = buf_.data() + pos;
            if (f[0] != mp::kFrame0 || f[1] != mp::kFrame1) { ++pos; continue; }
            const std::size_t len = f[3] | (f[4] << 8);
            if (len > kMaxFrame) { ++pos; continue; }
            if (buf_.size() - pos < mp::kFrameHeader + len) break;          // wait for the rest
            const std::uint16_t crc = static_cast<std::uint16_t>(f[9] | (f[10] << 8));
            if (mp::crc16(f + mp::kFrameHeader, len, mp::crc16(f + 2, 7)) != crc) { ++pos; continue; }
            const int sid = f[2];
            const std::uint32_t at = static_cast<std::uint32_t>(f[5] | (f[6] << 8) | (f[7] << 16)) |
                                     (static_cast<std::uint32_t>(f[8]) << 24);
            pos += mp::kFrameHeader + len;
            if (sid != sid_) {
                // Another session: only its very first frame starts it
                if (at != 0) continue;
                sid_ = sid;
                expected_ = 0;
                acked_ = 0;
                gap_ = false;
                heartbeat_ = false;
                ++sessions_;
            }
            lastFrame_ = now;
            if (len == 0) heartbeat_ = true;     // HIPI sends these when idle
            if (at == expected_) {
                out(f + mp::kFrameHeader, len);
                expected_ += static_cast<std::uint32_t>(mp::kFrameHeader + len);
                if (gap_) { gap_ = false; ++recovered_; }
            } else if (static_cast<std::int32_t>(at - expected_) > 0) {
                // Something before this frame is missing: ask for it again
                // (not too often -- the resend takes a moment to arrive)
                if (!gap_) { gap_ = true; gapSince_ = now; nakAt_ = -1; }
                if (now - nakAt_ > 0.15) { message('N', send); nakAt_ = now; }
            }
            else reack_ = true;   // one we already have, resent: our 'K' may have been lost
        }
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
        tick(now, send);
    }

    // Call regularly: acknowledges what arrived, repeats an unanswered
    // resend request
    void tick(double now, const Send& send) {
        if (sid_ < 0) return;
        if (gap_ && now - nakAt_ > 0.15) { message('N', send); nakAt_ = now; }
        const bool fresh = expected_ != acked_ && (expected_ - acked_ >= 2048 || now - ackAt_ > 0.02);
        const bool again = reack_ && now - ackAt_ > 0.05;      // a resend of what we have
        if (fresh || again) {
            reack_ = false;
            message('K', send);
            acked_ = expected_;
            ackAt_ = now;
        }
    }

    // A gap HIPI hasn't filled for a while (it gave up, or restarted):
    // time to ask for a whole new session
    bool stuck(double now) const { return gap_ && now - gapSince_ > 2.0; }
    // HIPI sends an empty frame every second when idle (newer firmware --
    // only relied on once one has been seen): silence means it ended the
    // session (e.g. we were suspended and it gave up on us)
    bool silent(double now) const { return sid_ >= 0 && heartbeat_ && now - lastFrame_ > 3.5; }
    bool inSession() const { return sid_ >= 0; }
    int sid() const { return sid_; }
    unsigned long long recovered() const { return recovered_; }
    unsigned long long sessions() const { return sessions_; }

private:
    static constexpr std::size_t kMaxFrame = 0x2000;   // HIPI sends at most 512 bytes
    void message(std::uint8_t type, const Send& send) {
        if (!send) return;
        const std::uint8_t m[6] = { type, static_cast<std::uint8_t>(sid_),
            static_cast<std::uint8_t>(expected_), static_cast<std::uint8_t>(expected_ >> 8),
            static_cast<std::uint8_t>(expected_ >> 16), static_cast<std::uint8_t>(expected_ >> 24) };
        send(m, sizeof m);
    }

    std::vector<std::uint8_t> buf_;
    int sid_ = -1;
    std::uint32_t expected_ = 0;     // where the next frame must start
    std::uint32_t acked_ = 0;        // last position acknowledged
    double ackAt_ = 0;
    double lastFrame_ = 0;
    bool heartbeat_ = false, reack_ = false;
    bool gap_ = false;
    double gapSince_ = 0, nakAt_ = -1;
    unsigned long long recovered_ = 0, sessions_ = 0;
};
