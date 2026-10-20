// deframer_test.cpp -- the viewer's end of the mirror link (deframer.hpp):
// when does it notice that HIPI has gone quiet?
//
//   make deframer_test && ./deframer_test   (in tools/hipiview)
#include "../deframer.hpp"
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
// One frame of session sid at stream position pos, n payload bytes
std::vector<std::uint8_t> frame(std::uint8_t sid, std::uint32_t pos, std::size_t n) {
    std::vector<std::uint8_t> f(hipi::mirror::kFrameHeader + n, 0x11);
    hipi::mirror::frameHeader(f.data(), sid, pos, f.data() + hipi::mirror::kFrameHeader, n);
    return f;
}
}  // namespace

int main() {
    auto out = [](const std::uint8_t*, std::size_t) {};
    // A session that was busy from its start: frames, never a heartbeat
    {
        Link link;
        link.reset();
        std::uint32_t pos = 0;
        double t = 0;
        for (int i = 0; i < 20; ++i, t += 0.05) {
            const auto f = frame(5, pos, 100);
            link.feed(f.data(), f.size(), t, out, nullptr);
            pos += static_cast<std::uint32_t>(f.size());
        }
        check(link.inSession() && !link.silent(t + 3.0), "busy session, 3 s without a frame: not silent yet");
        // HIPI ended the session (the link fell behind): nothing more comes.
        // Before, without a heartbeat seen this was waited for for ever.
        check(link.silent(t + 5.5), "... 5 s without a frame: silent, even without a heartbeat seen");
    }
    // Heartbeats seen in an earlier session on the same port: 3.5 s is enough
    {
        Link link;
        link.reset();
        auto hb = frame(1, 0, 0);
        link.feed(hb.data(), hb.size(), 0.0, out, nullptr);
        link.reset();                                   // a new session asked for
        const auto f = frame(2, 0, 100);
        link.feed(f.data(), f.size(), 1.0, out, nullptr);
        check(!link.silent(4.0) && link.silent(4.6), "heartbeats known from before: silent after 3.5 s");
    }
    // Idle with heartbeats: never silent
    {
        Link link;
        link.reset();
        std::uint32_t pos = 0;
        for (int s = 0; s < 20; ++s) {
            const auto f = frame(3, pos, 0);
            link.feed(f.data(), f.size(), s * 1.0, out, nullptr);
            pos += static_cast<std::uint32_t>(f.size());
        }
        check(!link.silent(19.9), "a heartbeat every second: not silent");
    }
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
