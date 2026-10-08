// link_sim.cpp -- the display mirror end to end, on the PC, over a lossy
// "USB": HIPI's real display_mirror.cpp (with TinyUSB stood in by the
// functions below) drives a pseudo-terminal, the real hipiview binary
// reads it, and the "USB" loses one 64-byte packet in `loss` (default
// 1/50). Afterwards hipiview's saved stream is replayed here through the
// viewer's own link code and emulator, and compared with the emulated
// chip: the picture must be identical, losses or not.
//
//   make link_sim && ./link_sim [loss-probability] [seconds] [stall]
//
// "stall": hipiview is frozen (SIGSTOP) for a while midway -- the port
// stays open but nobody reads it, as when hipiview is suspended. HIPI
// must notice, stop mirroring (no stalls of its main loop beyond the
// first one), and pick up again by itself when hipiview comes back.
#include "LT7683.hpp"
#include "Screen.hpp"
#include "display_mirror.h"
#include "tusb.h"
#include "pico/time.h"
#include "../lt7683_emu.hpp"
#include "../deframer.hpp"
#include "../png.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <pty.h>
#include <random>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {
LT7683Emu chip;
int master = -1;
double lossRate = 0.02;
std::mt19937 rng(1234);
std::deque<std::uint8_t> txFifo, rx;
std::vector<std::uint8_t> onWire;    // a packet the host hasn't taken yet
std::size_t sentPackets = 0, lostPackets = 0;
const auto t0 = std::chrono::steady_clock::now();
}

// ── TinyUSB stand-ins ───────────────────────────────────────────────────
absolute_time_t get_absolute_time() {
    return static_cast<absolute_time_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count());
}
bool tud_cdc_n_connected(std::uint8_t) { return true; }
static void pullRx() {
    std::uint8_t b[256];
    for (;;) {
        const ssize_t n = ::read(master, b, sizeof b);
        if (n <= 0) break;
        rx.insert(rx.end(), b, b + n);
    }
}
std::uint32_t tud_cdc_n_available(std::uint8_t) { pullRx(); return static_cast<std::uint32_t>(rx.size()); }
std::uint32_t tud_cdc_n_read(std::uint8_t, void* buf, std::uint32_t n) {
    std::uint32_t k = 0;
    auto* p = static_cast<std::uint8_t*>(buf);
    while (k < n && !rx.empty()) { p[k++] = rx.front(); rx.pop_front(); }
    return k;
}
std::uint32_t tud_cdc_n_write_available(std::uint8_t) { return static_cast<std::uint32_t>(1024 - txFifo.size()); }
std::uint32_t tud_cdc_n_write(std::uint8_t, const void* buf, std::uint32_t n) {
    const std::uint32_t k = std::min<std::uint32_t>(n, tud_cdc_n_write_available(0));
    const auto* p = static_cast<const std::uint8_t*>(buf);
    txFifo.insert(txFifo.end(), p, p + k);
    return k;
}
std::uint32_t tud_cdc_n_write_flush(std::uint8_t) { return 0; }
bool tud_cdc_n_write_clear(std::uint8_t) { txFifo.clear(); return true; }
// One 64-byte packet per call -- sometimes lost on the way. Like real
// USB, a packet the host doesn't take (nobody reads the port) waits.
void tud_task() {
    pullRx();
    if (onWire.empty()) {
        if (txFifo.empty()) return;
        while (onWire.size() < 64 && !txFifo.empty()) { onWire.push_back(txFifo.front()); txFifo.pop_front(); }
        ++sentPackets;
        if (std::uniform_real_distribution<double>(0, 1)(rng) < lossRate) { ++lostPackets; onWire.clear(); return; }
    }
    const ssize_t w = ::write(master, onWire.data(), onWire.size());
    if (w > 0) onWire.erase(onWire.begin(), onWire.begin() + w);
}

// ── The chip, answering like the real one ───────────────────────────────
class ChipTransport : public hipi::RA8875Transport {
public:
    void csLow() override { first_ = true; }
    void csHigh() override {}
    void rstLow() override {}
    void rstHigh() override {}
    void spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) override {
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t b = tx ? tx[i] : 0;
            if (first_) { type_ = b & 0xC0; first_ = false; if (rx) rx[i] = 0; continue; }
            std::uint8_t r = 0;
            switch (type_) {
            case 0x00: chip.hostCmd(b); break;
            case 0x80: chip.hostWrite(b); break;
            case 0xC0: r = chip.hostRead(); break;
            default:   r = chip.hostStatus(); break;
            }
            if (rx) rx[i] = r;
        }
    }
    void delayMs(std::uint32_t) override {}
    void delayUs(std::uint32_t) override {}
private:
    bool first_ = true;
    std::uint8_t type_ = 0;
};

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

int main(int argc, char** argv) {
    if (argc > 1) lossRate = std::atof(argv[1]);
    const double seconds = argc > 2 ? std::atof(argv[2]) : 8.0;
    const bool stall = argc > 3 && std::strcmp(argv[3], "stall") == 0;
    std::uint8_t font[256][16];
    for (int c = 0; c < 256; ++c) for (int r = 0; r < 16; ++r) font[c][r] = static_cast<std::uint8_t>((c * 7 + r * 13) | 0x81);
    chip.setBuiltinFont(font);

    int slave = -1;
    char name[128];
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) { std::perror("openpty"); return 1; }
    termios tio{};
    tcgetattr(master, &tio);
    cfmakeraw(&tio);
    tcsetattr(master, TCSANOW, &tio);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    std::remove("link_sim.hvlog");
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("HOME", "/tmp", 1);
        execl("./hipiview", "hipiview", name, "--log", "link_sim.hvlog", static_cast<char*>(nullptr));
        std::_Exit(127);
    }

    ChipTransport inner;
    hipi::MirrorTransport mirror(inner);
    hipi::LT7683 d(mirror, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    hipi::displayMirror_init(&d);
    hipi::displayMirror_setEnabled(true);    // Settings -> Screen -> Mirror to PC
    // A "picture" layer, as HIPI registers the Tape view
    d.beginLayerDraw(hipi::LT7683::kTapeLayerAddr);
    d.fillRect(0, 0, 1024, 600, 0x18E3);
    for (int i = 0; i < 40; ++i) d.fillCircle(30 + i * 25, 300, 20 + i % 7, static_cast<std::uint16_t>(i * 1597));
    d.endOverlayDraw();
    hipi::displayMirror_addLayer(hipi::LT7683::kTapeLayerAddr, 1024 * 600 * 2, 0, 1, 0x5151);
    hipi::Screen screen(&d, 0x07E0, 1, 200, 1024);

    // Run: HIPI's main loop, drawing now and then
    int line = 0;
    double nextDraw = 1.0;
    bool frozen = false;
    double frozenAt = 0, lastLoop = now(), maxLoopGap = 0;
    int longStalls = 0, lateStalls = 0;
    while (now() < seconds) {
        tud_task();                      // like HIPI's main loop
        // The setting switched off and on again midway: the session must
        // stop, and start again by itself (hipiview is still connected)
        if (!stall && now() > seconds * 0.5 && now() < seconds * 0.6) hipi::displayMirror_setEnabled(false);
        else hipi::displayMirror_setEnabled(true);
        if (stall && !frozen && now() > seconds * 0.3 && now() < seconds * 0.4) { kill(pid, SIGSTOP); frozen = true; frozenAt = now(); }
        if (stall && frozen && now() > seconds * 0.7) { kill(pid, SIGCONT); frozen = false; }
        const double t = now();
        if (t - lastLoop > 0.1) {
            ++longStalls;
            if (frozen && t - frozenAt > 3.0) ++lateStalls;      // still stalling long after the freeze
        }
        maxLoopGap = std::max(maxLoopGap, t - lastLoop);
        lastLoop = t;
        hipi::displayMirror_poll();
        if (now() > nextDraw) {
            nextDraw = now() + 0.03;
            char buf[64];
            std::snprintf(buf, sizeof buf, "Line %d, time %.2f s", line++, now());
            screen.pr_str(buf);
            if (line % 25 == 0) d.copyLayerToPanel(hipi::LT7683::kTapeLayerAddr, 0, 200, 1024, 200);
            if (line % 40 == 0) {
                d.beginOverlayDraw();
                d.fillRoundRect(300, 150, 400, 260, 12, static_cast<std::uint16_t>(line * 977));
                d.endOverlayDraw();
                d.showPipOverlay(300, 150, 400, 260);
            }
            if (line % 40 == 20) d.hidePipOverlay();
        }
        usleep(100);
    }
    // Let everything drain
    const double end = now() + 2.0;
    while (now() < end) { tud_task(); hipi::displayMirror_poll(); usleep(100); }
    kill(pid, SIGINT);
    int st = 0;
    waitpid(pid, &st, 0);

    // Replay hipiview's saved stream through the viewer's own link + emulator
    LT7683Emu viewer;
    std::vector<std::uint8_t> asset;
    // hipiview's own cache (HOME=/tmp for the child above)
    viewer.loadAsset = [&](std::uint32_t id, std::vector<std::uint8_t>& out) {
        if (!asset.empty()) { out = asset; return true; }
        char path[64];
        std::snprintf(path, sizeof path, "/tmp/.cache/hipiview/%08x.asset", id);
        FILE* af = std::fopen(path, "rb");
        if (!af) return false;
        // "HVA2" size crc, then the picture (hipiview checked it already)
        std::uint8_t head[12];
        const bool ok = std::fread(head, 1, sizeof head, af) == sizeof head;
        out.assign(1024 * 600 * 2 + 1, 0);
        out.resize(ok ? std::fread(out.data(), 1, out.size(), af) : 0);
        std::fclose(af);
        return ok;
    };
    viewer.saveAsset = [&](std::uint32_t, const std::vector<std::uint8_t>& b) { asset = b; };
    FILE* f = std::fopen("link_sim.hvlog", "rb");
    if (!f) { std::printf("FAIL no log from hipiview\n"); return 1; }
    char magic[13];
    if (std::fread(magic, 1, 13, f) != 13) return 1;
    Link link;
    std::uint32_t hdr[2];
    while (std::fread(hdr, sizeof hdr, 1, f) == 1) {
        std::vector<std::uint8_t> b(hdr[1]);
        if (std::fread(b.data(), 1, b.size(), f) != b.size()) break;
        link.feed(b.data(), b.size(), hdr[0] / 1000.0, [&](const std::uint8_t* p, std::size_t n) { viewer.feed(p, n); }, nullptr);
    }
    std::fclose(f);

    std::vector<std::uint32_t> a(1024 * 600), b(1024 * 600);
    chip.render(a.data(), 0.0);
    viewer.render(b.data(), 0.0);
    long diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
    std::printf("USB packets: %zu sent, %zu lost (%.1f %%); link: %llu gaps recovered, %llu sessions\n",
                sentPackets, lostPackets, 100.0 * lostPackets / std::max<std::size_t>(1, sentPackets),
                link.recovered(), link.sessions());
    writePng("link_sim_viewer.png", b.data(), 1024, 600);
    std::printf("main loop: longest pause %.0f ms, %d pauses over 100 ms (%d of them > 3 s into the freeze)\n",
                maxLoopGap * 1000, longStalls, lateStalls);
    if (stall && lateStalls > 0) {
        std::printf("FAIL HIPI kept stalling while nobody read the port\n");
        return 1;
    }
    if (diff) {
        writePng("link_sim_chip.png", a.data(), 1024, 600);
        std::printf("FAIL %ld pixels differ\n", diff);
        return 1;
    }
    std::printf("ok   the viewer shows exactly what the chip shows\n");
    return 0;
}
