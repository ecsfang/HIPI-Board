// link_sim.cpp -- the display mirror end to end, on the PC, over a lossy
// "USB": HIPI's real display_mirror.cpp (with TinyUSB stood in by the
// functions below) drives a pseudo-terminal, the real hipiview binary
// reads it, and the "USB" loses one 64-byte packet in `loss` (default
// 1/50). Afterwards hipiview's saved stream is replayed here through the
// viewer's own link code and emulator, and compared with the emulated
// chip: the picture must be identical, losses or not.
//
//   make link_sim && ./link_sim [loss-probability] [seconds] [stall|input]
//
// "input": hipiview plays mouse and key events (--test-input) as with
// Mirror to PC < Control >; checks that HIPI gets them as touches and
// buttons -- and none while the setting is View.
//
// "upload": a LIF volume dropped on hipiview's window (--test-input: a
// "drop", the "Replace?" answered yes) goes over the lossy link into HIPI's
// receiver (mirror_upload.cpp) and a stand-in drive; the file on the
// "SD card" (link_sim_sd/lif/) must be the one dropped, the old cassette
// taken out first.
//
// "download": a drive's cassette to the PC, with Mirror to PC < View >:
// asked for with hipiview's M key, then once more from "HIPI's menu"
// (displayMirror_sendMedia()); hipiview saves them (--save-to
// link_sim_saved/) -- both must be the drive's file, byte for byte.
//
// "busy": HIPI's own main loop held up for 4 s midway (a long SD card
// access ...), right after drawing something: the viewer must ask "still
// there?" and go on with the same session -- one session only, no whole
// new picture.
//
// "idleburst": idle a while (only empty frames, acknowledged once a
// second), then a picture bigger than the ring at once: HIPI must wait for
// the viewer, not take the last ack (up to a second old) for "nobody's
// reading" and end the session -- one session only.
//
// "stall": hipiview is frozen (SIGSTOP) for a while midway -- the port
// stays open but nobody reads it, as when hipiview is suspended. HIPI
// must notice, stop mirroring (no stalls of its main loop beyond the
// first one), and pick up again by itself when hipiview comes back.
#include "LT7683.hpp"
#include "Screen.hpp"
#include "display_mirror.h"
#include "mirror_upload.h"
#include "mirror_download.h"
#include "lif_file.h"
#include "lif_dir.h"
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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
class ChipTransport : public hipi::DisplayTransport {
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
    const bool uploadTest = argc > 3 && std::strcmp(argv[3], "upload") == 0;
    const bool downloadTest = argc > 3 && std::strcmp(argv[3], "download") == 0;
    const bool inputTest = (argc > 3 && std::strcmp(argv[3], "input") == 0) || uploadTest || downloadTest;
    // burst: more drawing than the link can carry, from the very start (so
    // the viewer sees no heartbeat first): the encoder gives up, and HIPI
    // must send the screen again by itself -- the viewer is still there
    const bool burst = argc > 3 && std::strcmp(argv[3], "burst") == 0;
    const bool busy = argc > 3 && std::strcmp(argv[3], "busy") == 0;
    const bool idleBurst = argc > 3 && std::strcmp(argv[3], "idleburst") == 0;
    int idleBursts = 0;
    double nextIdleBurst = seconds * 0.3;
    bool busyDone = false;
    // upload: the file dropped -- a LIF volume of 96 kB
    std::vector<std::uint8_t> tape(96 * 1024);
    for (std::size_t k = 0; k < tape.size(); ++k) tape[k] = static_cast<std::uint8_t>(k * 2654435761u >> 13);
    tape[0] = 0x80; tape[1] = 0x00;
    for (int k = 8; k < 20; ++k) tape[k] = 0;
    tape[11] = 2;                                            // directory at block 2,
    tape[19] = 4;                                            // 4 blocks long, empty
    std::fill(tape.begin() + 2 * 256, tape.begin() + 6 * 256, 0xFF);
    // download: VECTOP on the cassette too -- sent on its own later (the
    // Media map's Save on PC), as lifget -r gives it
    std::vector<std::uint8_t> vectopRaw;
    if (downloadTest) {
        std::ifstream rf("test/data/vectop.raw", std::ios::binary);
        vectopRaw.assign(std::istreambuf_iterator<char>(rf), std::istreambuf_iterator<char>());
        hipi::LifFileImage img;
        std::string why;
        hipi::lifFileFromPc("vectop.raw", vectopRaw, img, why);
        hipi::lifAddFile([&](std::uint32_t b, std::uint8_t* o) { std::copy_n(tape.begin() + b * 256, 256, o); return true; },
                         [&](std::uint32_t b, const std::uint8_t* d) { std::copy_n(d, 256, tape.begin() + b * 256); return true; },
                         static_cast<std::uint32_t>(tape.size() / 256), img.bytes.data(),
                         [&](std::uint32_t b, std::uint8_t* o) { std::copy_n(img.bytes.begin() + 32 + b * 256, 256, o); return true; },
                         false, why);
        std::filesystem::remove_all("link_sim_saved");
        std::filesystem::create_directories("link_sim_saved");
        std::FILE* f = std::fopen("link_sim_input.txt", "w");
        std::fputs("2.0 key m\n", f);
        std::fclose(f);
    } else if (uploadTest) {
        std::FILE* tf = std::fopen("link_sim_tape.dat", "wb");
        std::fwrite(tape.data(), 1, tape.size(), tf);
        std::fclose(tf);
        std::FILE* f = std::fopen("link_sim_input.txt", "w");
        // The cassette, then two HP-41 programs onto it, dropped at once
        // ("Add all" for the first: the second isn't asked about)
        {
            std::ifstream rf("test/data/vectop.raw", std::ios::binary);
            std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
            raw[5] = 'Q';                                    // LBL "SQH"
            std::ofstream("link_sim_second.raw", std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            raw[5] = 'R';                                    // LBL "SRH"
            std::ofstream("link_sim_third.raw", std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            raw[5] = 'X';                                    // (a changed VECTOP: must not go on)
            std::ofstream("vectop.raw", std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        }
        std::fputs("2.0 answer yes\n2.0 drop link_sim_tape.dat\n"
                   "6.0 answer all\n6.0 drop test/data/vectop.raw\n6.0 drop link_sim_input.txt\n6.0 drop link_sim_second.raw\n"   // (the .txt: refused, said at the end)
                   // Again: a new one ("Add all"), then VECTOP once more -- on the
                   // cassette already: asked about all the same ("Add all" never
                   // replaces), and skipped
                   "11.0 answer all\n11.0 answer no\n11.0 drop link_sim_third.raw\n11.0 drop vectop.raw\n", f);
        std::fclose(f);
    } else if (inputTest) {
        // Times in seconds from when hipiview's picture is in step
        std::FILE* f = std::fopen("link_sim_input.txt", "w");
        std::fputs("0.5 down 100 200\n0.52 up\n"                       // a quick click
                   "1.0 down 800 300\n1.05 move 700 300\n1.1 move 600 300\n1.15 move 500 300\n"
                   "1.2 move 400 300\n1.25 move 300 300\n1.3 up\n"     // a swipe
                   "1.6 key Return\n1.65 keyup Return\n"
                   "2.0 key Up\n3.0 keyup Up\n"                         // held a second
                   "3.3 wheel 1\n3.4 wheel -1\n3.5 key Tab\n3.55 keyup Tab\n3.6 key Right\n3.7 key Left\n"
                   "8.0 down 50 50\n8.2 up\n8.3 key Return\n8.35 keyup Return\n", f);   // (View: ignored)
        std::fclose(f);
    }
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
        if (downloadTest)
            execl("./hipiview", "hipiview", name, "--log", "link_sim.hvlog", "--test-input", "link_sim_input.txt",
                  "--save-to", "link_sim_saved", static_cast<char*>(nullptr));
        else if (inputTest)
            execl("./hipiview", "hipiview", name, "--log", "link_sim.hvlog", "--test-input", "link_sim_input.txt",
                  static_cast<char*>(nullptr));
        else
            execl("./hipiview", "hipiview", name, "--log", "link_sim.hvlog", static_cast<char*>(nullptr));
        std::_Exit(127);
    }

    // upload: HIPI's SD card is link_sim_sd/ (hipiview, forked above, stays here)
    if (uploadTest) {
        std::filesystem::remove_all("link_sim_sd");
        std::filesystem::create_directories("link_sim_sd/lif");
        if (chdir("link_sim_sd") != 0) { std::perror("link_sim_sd"); return 1; }
        std::FILE* old = std::fopen("lif/OLD.DAT", "wb");
        std::fputs("the old cassette", old);
        std::fclose(old);
    }
    struct SimDrive : hipi::UploadDrive {
        std::string media = "OLD.DAT";
        std::vector<std::string> calls;
        std::string name() const override { return "TFDRIVE"; }
        std::string mediaFile() const override { return media; }
        bool quiet() const override { return true; }
        void open() override { calls.push_back("open " + media); }
        void insert(const std::string& f) override { media = f; calls.push_back("insert " + f); }
        bool readBlock(std::uint32_t block, std::uint8_t* out) override {
            std::ifstream in("lif/" + media, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(block) * 256);
            in.read(reinterpret_cast<char*>(out), 256);
            return in.gcount() == 256;
        }
        std::uint32_t fileSize() const override {
            std::error_code ec;
            const auto n = std::filesystem::file_size("lif/" + media, ec);
            return ec ? 0 : static_cast<std::uint32_t>(n);
        }
    };
    auto simDrive = std::make_shared<SimDrive>();

    ChipTransport inner;
    hipi::MirrorTransport mirror(inner);
    hipi::LT7683 d(mirror, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    hipi::displayMirror_init(&d);
    hipi::displayMirror_setMode(inputTest && !downloadTest ? hipi::MirrorMode::Control : hipi::MirrorMode::View);
    // download: the drive whose cassette goes to the PC (its file: `tape`)
    struct SimDownDrive : hipi::DownloadDrive {
        const std::vector<std::uint8_t>* file = nullptr;
        std::string name() const override { return "TFDRIVE"; }
        std::string mediaFile() const override { return "SIMTAPE.DAT"; }
        bool quiet() const override { return true; }
        std::uint32_t changes() const override { return 0; }
        std::uint32_t fileSize() const override { return static_cast<std::uint32_t>(file->size()); }
        bool readBlock(std::uint32_t block, std::uint8_t* out) override {
            for (std::uint32_t i = 0; i < 256; ++i) out[i] = block * 256 + i < file->size() ? (*file)[block * 256 + i] : 0xFF;
            return true;
        }
    };
    if (downloadTest) {
        auto drive = std::make_shared<SimDownDrive>();
        drive->file = &tape;
        hipi::DownloadEnv env;
        env.drive = [drive]() -> std::shared_ptr<hipi::DownloadDrive> { return drive; };
        env.notNow = [] { return std::string(); };
        env.nowMs = [] { return static_cast<std::uint64_t>(now() * 1000); };
        hipi::displayMirror_setDownloadEnv(env);
    }
    bool menuSent = false;
    if (uploadTest) {
        hipi::UploadEnv env;
        env.drive = [&]() -> std::shared_ptr<hipi::UploadDrive> { return simDrive; };
        env.notNow = [] { return std::string(); };
        env.inOtherDrive = [](const std::string&) { return std::string(); };
        env.nowMs = [] { return static_cast<std::uint64_t>(now() * 1000); };
        hipi::displayMirror_setUploadEnv(env);
    }
    // What arrives from the viewer's mouse and keys
    struct InputEvent { double t; char kind; int a, b, c; };
    std::vector<InputEvent> inputs;
    // Input must only arrive from the top of displayMirror_poll() -- never
    // in the middle of a drawing (when the ring is full, the mirror reads
    // the viewer from inside RingSink::put(); a handler that draws must
    // not run there)
    bool drawing = false;
    int nestedInput = 0;
    hipi::displayMirror_setInputHandlers(
        [&](bool down, std::uint16_t x, std::uint16_t y) { nestedInput += drawing; inputs.push_back({ now(), 'T', down, x, y }); },
        [&](std::uint8_t code, bool down) { nestedInput += drawing; inputs.push_back({ now(), 'B', code, down, 0 }); });
    std::vector<std::uint16_t> bigPicture(320 * 240);
    for (std::size_t k = 0; k < bigPicture.size(); ++k) bigPicture[k] = static_cast<std::uint16_t>(k * 2654435761u >> 16);
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
        if (uploadTest) {
            // (Control all the time)
        } else if (downloadTest) {
            // (View all the time.) Later, once more from "HIPI's menu"
            if (!menuSent && now() > seconds * 0.5) {
                std::string why;
                menuSent = true;
                if (!hipi::displayMirror_sendMedia(why)) std::printf("FAIL download: Save media on PC: %s\n", why.c_str());
            }
            // ... and one file of it (the Media map's file box)
            static bool fileSent = false;
            if (!fileSent && now() > seconds * 0.75) {
                std::string why;
                fileSent = true;
                if (!hipi::displayMirror_sendFile("VECTOP", why)) std::printf("FAIL download: Save on PC (VECTOP): %s\n", why.c_str());
            }
        } else if (inputTest) {
            // After "previous view" (the last key before the View part): View
            static bool viewOnly = false;
            if (!viewOnly) for (const auto& e : inputs) if (e.kind == 'B' && e.a == 7) viewOnly = true;
            if (viewOnly) hipi::displayMirror_setMode(hipi::MirrorMode::View);
        } else if (!stall && !burst && !busy && !idleBurst && now() > seconds * 0.5 && now() < seconds * 0.6) {
            hipi::displayMirror_setMode(hipi::MirrorMode::Off);
        } else {
            hipi::displayMirror_setMode(hipi::MirrorMode::View);
        }
        if (burst) {
            // The link worse while HIPI draws too much: the ring stays full
            // and the encoder gives up; afterwards as usual again
            static const double normalLoss = lossRate;
            lossRate = now() > 0.3 && now() < seconds * 0.4 ? 0.3 : normalLoss;
        }
        if (idleBurst && idleBursts < 3 && now() > nextIdleBurst) {
            // (nothing else is drawn in this mode: idle in between)
            ++idleBursts;
            nextIdleBurst = now() + 2.5;
            for (std::size_t k = 0; k < bigPicture.size(); k += 31) bigPicture[k] = static_cast<std::uint16_t>(bigPicture[k] + 977);
            d.drawBitmap565(200, 100, 320, 240, bigPicture.data());   // 150 kB at once
        }
        if (busy && !busyDone && now() > seconds * 0.4) {
            // Drawing, then HIPI held up 4 s: nothing read, nothing sent
            busyDone = true;
            d.fillRect(100, 100, 600, 300, 0x1234);
            d.drawBitmap565(40, 120, 64, 64, bigPicture.data());   // (8 kB: some of it still unsent)
            const double until = now() + 4.0;
            while (now() < until) usleep(1000);
        }
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
        // idleburst: nothing drawn but the bursts -- idle in between
        if (now() > nextDraw && !idleBurst) {
            drawing = true;
            nextDraw = now() + 0.03;
            // (input test: big pictures too, so the ring fills up while drawing)
            if (inputTest && !uploadTest && !downloadTest && line % 20 == 0) d.drawBitmap565(40, 40, 320, 240, bigPicture.data());
            if (burst && now() > 0.3 && now() < seconds * 0.4) {
                for (std::size_t k = 0; k < bigPicture.size(); k += 97) bigPicture[k] = static_cast<std::uint16_t>(bigPicture[k] * 31 + line);
                for (int k = 0; k < 4; ++k) d.drawBitmap565(40 + k * 160, 40 + (line % 5) * 20, 320, 240, bigPicture.data());
            }
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
            drawing = false;
        }
        usleep(100);
    }
    // Let everything drain -- the way HIPI says goodbye before Bootsel
    // mode, then a little more
    hipi::displayMirror_goodbye(1500);
    const double end = now() + 0.5;
    while (now() < end) { tud_task(); hipi::displayMirror_poll(); usleep(100); }
    kill(pid, SIGINT);
    if (uploadTest && chdir("..") != 0) return 1;
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
        // (bit 31 of the length: a touch from the viewer's mouse, not HIPI's stream)
        std::vector<std::uint8_t> b(hdr[1] & 0x7FFFFFFFu);
        if (std::fread(b.data(), 1, b.size(), f) != b.size()) break;
        if (hdr[1] & 0x80000000u) continue;
        link.feed(b.data(), b.size(), hdr[0] / 1000.0, [&](const std::uint8_t* p, std::size_t n) {
            if (p == nullptr) viewer.desync();       // a new session (see Link::feed)
            else viewer.feed(p, n);
        }, nullptr);
    }
    std::fclose(f);

    std::vector<std::uint32_t> a(1024 * 600), b(1024 * 600);
    chip.render(a.data(), 0.0);
    viewer.render(b.data(), 0.0);
    long diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
    if (!viewer.saidBye()) { std::printf("FAIL the goodbye (Bootsel mode) didn't reach the viewer\n"); return 1; }
    std::printf("USB packets: %zu sent, %zu lost (%.1f %%); link: %llu gaps recovered, %llu sessions\n",
                sentPackets, lostPackets, 100.0 * lostPackets / std::max<std::size_t>(1, sentPackets),
                link.recovered(), link.sessions());
    writePng("link_sim_viewer.png", b.data(), 1024, 600);
    std::printf("main loop: longest pause %.0f ms, %d pauses over 100 ms (%d of them > 3 s into the freeze)\n",
                maxLoopGap * 1000, longStalls, lateStalls);
    if (downloadTest) {
        int fails = 0;
        auto fail = [&](const std::string& what) { std::printf("FAIL download: %s\n", what.c_str()); ++fails; };
        auto read = [](const char* p) {
            std::ifstream in(p, std::ios::binary);
            return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const auto first = read("link_sim_saved/SIMTAPE.DAT");
        const auto second = read("link_sim_saved/SIMTAPE (2).DAT");
        if (first.empty()) fail("M key: link_sim_saved/SIMTAPE.DAT not saved");
        else if (first != tape) fail("M key: SIMTAPE.DAT differs from the drive's file");
        if (second.empty()) fail("Save media on PC: link_sim_saved/SIMTAPE (2).DAT not saved");
        else if (second != tape) fail("Save media on PC: SIMTAPE (2).DAT differs from the drive's file");
        // VECTOP on its own: the program and its checksum (lifget -r)
        std::vector<std::uint8_t> want = vectopRaw;
        unsigned sum = 0;
        for (auto b : vectopRaw) sum += b;
        want.push_back(static_cast<std::uint8_t>(sum));
        const auto one = read("link_sim_saved/VECTOP.raw");
        if (one.empty()) fail("Save on PC: link_sim_saved/VECTOP.raw not saved");
        else if (one != want) fail("Save on PC: VECTOP.raw isn't the program + checksum (as lifget -r)");
        if (fails) return 1;
        std::printf("ok   download: twice %zu bytes over the lossy link (M key, HIPI's menu), saved whole; VECTOP.raw on its own\n", tape.size());
    } else if (uploadTest) {
        int fails = 0;
        auto fail = [&](const std::string& what) { std::printf("FAIL upload: %s\n", what.c_str()); ++fails; };
        std::ifstream in("link_sim_sd/lif/link_sim_tape.dat", std::ios::binary);
        const std::vector<std::uint8_t> got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (got.empty()) fail("lif/link_sim_tape.dat not on the SD card");
        // The cassette as dropped -- with VECTOP and LINKSIMSEC added after
        // each other (as lif_dir.cpp adds them, checked against lifutils)
        else {
            std::vector<std::uint8_t> want = tape;
            for (const char* p : { "test/data/vectop.raw", "link_sim_second.raw", "link_sim_third.raw" }) {
                std::ifstream rf(p, std::ios::binary);
                const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
                hipi::LifFileImage img;
                std::string why;
                hipi::lifFileFromPc(p, raw, img, why);
                hipi::lifAddFile([&](std::uint32_t b, std::uint8_t* o) { std::copy_n(want.begin() + b * 256, 256, o); return true; },
                                 [&](std::uint32_t b, const std::uint8_t* d) { std::copy_n(d, 256, want.begin() + b * 256); return true; },
                                 static_cast<std::uint32_t>(want.size() / 256), img.bytes.data(),
                                 [&](std::uint32_t b, std::uint8_t* o) { std::copy_n(img.bytes.begin() + 32 + b * 256, 256, o); return true; },
                                 false, why);
            }
            if (got != want) fail("lif/link_sim_tape.dat isn't the cassette dropped with VECTOP, LINKSIMSEC and LINKSIMTHI added (VECTOP not replaced)");
        }
        if (std::filesystem::exists("link_sim_sd/lif/.upload.tmp")) fail("the temporary file left behind");
        if (!std::filesystem::exists("link_sim_sd/lif/OLD.DAT")) fail("the old cassette's file gone");
        const std::vector<std::string> want = { "open OLD.DAT", "insert link_sim_tape.dat",
                                                "open link_sim_tape.dat", "insert link_sim_tape.dat",
                                                "open link_sim_tape.dat", "insert link_sim_tape.dat",
                                                "open link_sim_tape.dat", "insert link_sim_tape.dat" };
        if (simDrive->calls != want) {
            std::string c;
            for (const auto& x : simDrive->calls) c += " [" + x + "]";
            fail("the drive: expected [open OLD.DAT] [insert link_sim_tape.dat] [open ...] [insert ...], got" + c);
        }
        if (fails) return 1;
        std::printf("ok   upload: %zu bytes over the lossy link, in the drive, the old cassette out first; then programs (.raw) dropped several at once: added, an existing one asked about and skipped\n", tape.size());
    } else if (inputTest) {
        int fails = 0;
        auto fail = [&](const char* what) { std::printf("FAIL input: %s\n", what); ++fails; };
        auto find = [&](std::size_t from, char kind, int a, int b) -> std::size_t {
            for (std::size_t i = from; i < inputs.size(); ++i)
                if (inputs[i].kind == kind && inputs[i].a == a && (b < 0 || inputs[i].b == b)) return i;
            return inputs.size();
        };
        // The quick click: down at (100,200), lifted no sooner than 0.1 s later
        std::size_t i = find(0, 'T', 1, 100);
        if (i == inputs.size() || inputs[i].c != 200) fail("click: no press at (100,200)");
        else {
            const std::size_t u = find(i, 'T', 0, -1);
            if (u == inputs.size()) fail("click: never lifted");
            // (hipiview holds a click 100 ms; on the way, HIPI busy drawing
            // can take some of that away -- touch.cpp holds it 100 ms
            // again, from when the press arrives)
            else if (inputs[u].t - inputs[i].t < 0.03) fail("click: lifted at once (not held)");
        }
        // The swipe: from (800,300) to about (300,300), then lifted
        i = find(0, 'T', 1, 800);
        if (i == inputs.size()) fail("swipe: no press at (800,300)");
        else {
            std::size_t last = i;
            for (std::size_t k = i; k < inputs.size() && !(inputs[k].kind == 'T' && inputs[k].a == 0); ++k)
                if (inputs[k].kind == 'T') last = k;
            if (inputs[last].b > 320 || inputs[last].c != 300) fail("swipe: didn't get to (300,300)");
            if (find(last, 'T', 0, -1) == inputs.size()) fail("swipe: never lifted");
        }
        // Buttons
        if (find(0, 'B', 2, 1) == inputs.size() || find(0, 'B', 2, 0) == inputs.size()) fail("Return: no OK");
        int upRepeats = 0;
        const std::size_t up0 = find(0, 'B', 3, 1);
        for (std::size_t k = up0; k < inputs.size() && !(inputs[k].kind == 'B' && inputs[k].a == 3 && inputs[k].b == 0); ++k)
            if (inputs[k].kind == 'B' && inputs[k].a == 3 && inputs[k].b == 1) ++upRepeats;
        if (upRepeats < 4) fail("Up held 1 s: not repeated while held");
        if (find(up0, 'B', 3, 0) == inputs.size()) fail("Up: never released");
        if (find(0, 'B', 4, 1) == inputs.size()) fail("wheel: no Down");
        if (find(0, 'B', 6, 1) == inputs.size()) fail("Right: no next view");
        if (find(0, 'B', 7, 1) == inputs.size()) fail("Left: no previous view");
        if (find(0, 'B', 1, 1) == inputs.size()) fail("Tab: no Shift");
        // View: nothing gets through
        if (find(0, 'T', 1, 50) != inputs.size()) fail("View: a touch got through");
        if (nestedInput > 0) fail("input handed over in the middle of a drawing");
        const std::size_t v = find(0, 'B', 7, 1);
        if (v < inputs.size() && find(v + 1, 'B', 2, 1) != inputs.size()) fail("View: a button got through");
        std::printf("input: %zu events from the viewer%s\n", inputs.size(), fails ? "" : ", all as expected");
        if (fails) {
            for (const auto& e : inputs) std::printf("  %.3f %c %d %d %d\n", e.t, e.kind, e.a, e.b, e.c);
            return 1;
        }
    }
    if (idleBurst) {
        std::printf("idleburst: %d bursts after idling, %llu session(s) seen by the viewer\n", idleBursts, link.sessions());
        if (link.sessions() != 1) { std::printf("FAIL idleburst: a burst after idling ended the session\n"); return 1; }
    }
    if (busy) {
        std::printf("busy: %llu session(s) seen by the viewer\n", link.sessions());
        if (link.sessions() != 1) { std::printf("FAIL busy: HIPI held up 4 s cost a new session\n"); return 1; }
    }
    if (burst) {
        std::printf("burst: %llu sessions seen by the viewer\n", link.sessions());
        if (link.sessions() < 2) { std::printf("FAIL burst: the encoder never gave up (no test)\n"); return 1; }
    }
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
