// upload_test.cpp -- a LIF volume dropped on hipiview's Tape view, sent to
// HIPI: hipiview's side (upload.hpp) against HIPI's (src/mirror_upload.cpp)
// in one process, the "SD card" a folder, the drive a stand-in. Messages
// can be lost or damaged on the way, the drive can be busy.
//
//   make upload_test && ./upload_test   (in tools/hipiview)
#include "../upload.hpp"
#include "mirror_upload.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

std::uint64_t nowMs = 1000;

// The drive: what HIPI's would do (src/pico_main.cpp's DriveForUpload)
struct FakeDrive : hipi::UploadDrive {
    std::string nm = "TFDRIVE", media;
    bool isQuiet = true, ejected = false;
    int opens = 0, inserts = 0;
    std::string name() const override { return nm; }
    std::string mediaFile() const override { return media; }
    bool quiet() const override { return isQuiet; }
    void open() override { ++opens; ejected = true; }
    void insert(const std::string& f) override { ++inserts; media = f; ejected = false; }
};

// A LIF volume: header (0x8000, directory at block 2, 2 blocks), the rest
std::vector<std::uint8_t> lifVolume(std::size_t blocks, std::uint8_t fill) {
    std::vector<std::uint8_t> v(blocks * 256, fill);
    std::fill(v.begin(), v.begin() + 256, 0);
    v[0] = 0x80; v[1] = 0x00;
    std::memcpy(&v[2], "TEST  ", 6);
    v[11] = 2;                                  // directory start
    v[19] = 2;                                  // directory length
    return v;
}

std::vector<std::uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Both ends wired together; `loss` of the messages lost each way
struct Link {
    std::shared_ptr<FakeDrive> drive = std::make_shared<FakeDrive>();
    bool tapeShown = true;
    std::string notNow, other;
    std::deque<std::vector<std::uint8_t>> toHipi;
    std::deque<std::pair<std::uint8_t, std::vector<std::uint8_t>>> toViewer;
    std::mt19937 rng{ 1 };
    double loss = 0.0;
    int damageData = 0;                         // pieces to damage (after the 'F' CRC: the CRC-32 must catch it)
    hipi::UploadReceiver hipiSide;
    Uploader viewer;
    double t = 0;
    std::uint64_t bytesToHipi = 0;

    Link()
        : hipiSide(env(), [this](std::uint8_t k, const std::uint8_t* p, std::size_t n) {
              toViewer.emplace_back(k, std::vector<std::uint8_t>(p, p + n));
          }),
          viewer([this](std::vector<std::uint8_t> m) { toHipi.push_back(std::move(m)); }) {}

    hipi::UploadEnv env() {
        hipi::UploadEnv e;
        e.drive = [this]() -> std::shared_ptr<hipi::UploadDrive> { return tapeShown ? drive : nullptr; };
        e.notNow = [this] { return notNow; };
        e.inOtherDrive = [this](const std::string& f) { return f == "INDRIVE2.DAT" ? std::string("TFDRIVE2") : other; };
        e.nowMs = [] { return nowMs; };
        return e;
    }
    bool lost() { return loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < loss; }

    // 10 ms of both sides
    void step() {
        t += 0.01;
        nowMs += 10;
        while (!toHipi.empty()) {
            std::vector<std::uint8_t> m = std::move(toHipi.front());
            toHipi.pop_front();
            bytesToHipi += m.size();
            if (lost()) continue;
            const std::size_t len = static_cast<std::size_t>(m[3] | (m[4] << 8));
            if (m[2] == hipi::mirror::kUpData && damageData > 0 && len > 8) { m[5 + 6] ^= 0x40; --damageData; }
            hipiSide.handle(m[2], m.data() + 5, len);
        }
        hipiSide.poll();
        while (!toViewer.empty()) {
            auto r = std::move(toViewer.front());
            toViewer.pop_front();
            if (lost()) continue;
            viewer.onReply(r.first, r.second.data(), r.second.size(), t);
        }
        viewer.tick(t);
    }
    // Until the viewer wants an answer, or it's over
    void runUntilSettled(double maxSeconds) {
        const double end = t + maxSeconds;
        while (t < end) {
            step();
            if (viewer.step() == Uploader::Step::Confirm || !viewer.busy()) return;
        }
    }
};

}  // namespace

int main() {
    char cwd[512];
    if (getcwd(cwd, sizeof cwd) == nullptr) return 1;
    const std::string sd = std::string(cwd) + "/upload_sd";
    std::filesystem::remove_all(sd);
    std::filesystem::create_directories(sd + "/lif");
    if (chdir(sd.c_str()) != 0) return 1;
    std::ofstream("lif/GAMES.DAT") << "the old cassette";

    // ── Checked on the PC first ─────────────────────────────────────────
    {
        Link k;
        std::string why;
        check(!k.viewer.startWith("NOTES.TXT", lifVolume(4, 1), 0, why), "a .txt: refused on the PC (only .dat / .lif)");
        std::vector<std::uint8_t> bad(1024, 0x41);
        const bool started = k.viewer.startWith("BAD.DAT", bad, 0, why);
        std::printf("     (%s)\n", why.c_str());
        check(!started && why.find("LIF") != std::string::npos, "not a LIF volume: refused on the PC");
        check(k.toHipi.empty(), "... and HIPI wasn't even asked");
    }

    // ── An empty drive: the dropped file goes in ─────────────────────────
    {
        Link k;
        const std::vector<std::uint8_t> vol = lifVolume(512, 0x5A);    // a cassette, 128 kB
        std::string why;
        check(k.viewer.startWith("NEW.DAT", vol, k.t, why), "NEW.DAT dropped: a LIF volume, HIPI asked");
        k.runUntilSettled(3);
        check(k.viewer.step() == Uploader::Step::Confirm, "HIPI answers: the question for the user");
        std::printf("     \"%s\" [%s]\n", k.viewer.question().c_str(), k.viewer.yesText().c_str());
        check(k.viewer.question().find("TFDRIVE is empty") != std::string::npos && k.viewer.yesText() == "Put in",
              "... \"Put NEW.DAT into TFDRIVE? TFDRIVE is empty.\" [Put in]");
        check(!std::filesystem::exists("lif/NEW.DAT") && !std::filesystem::exists(hipi::UploadReceiver::kTempFile),
              "... nothing written before the answer");
        k.viewer.answer(true, k.t);
        k.runUntilSettled(30);
        std::printf("     %s  (%.2f s, %llu bytes sent)\n", k.viewer.status().c_str(), k.t,
                    static_cast<unsigned long long>(k.bytesToHipi));
        check(k.viewer.step() == Uploader::Step::Done, "sent: done");
        check(readFile("lif/NEW.DAT") == vol, "lif/NEW.DAT is the file, byte for byte");
        check(k.drive->media == "NEW.DAT" && k.drive->opens == 1 && k.drive->inserts == 1 && !k.drive->ejected,
              "TFDRIVE opened (as OPEN), NEW.DAT put in, the lid closed");
        check(!std::filesystem::exists(hipi::UploadReceiver::kTempFile), "the temporary file is gone");
    }

    // ── A cassette in: replaced -- once the drive is quiet ───────────────
    {
        Link k;
        k.drive->media = "GAMES.DAT";
        k.drive->isQuiet = false;                    // the HP-41 is reading from it
        const std::vector<std::uint8_t> vol = lifVolume(64, 0x33);
        std::string why;
        k.viewer.startWith("SPACE.LIF", vol, k.t, why);
        k.runUntilSettled(3);
        std::printf("     \"%s\" [%s]\n", k.viewer.question().c_str(), k.viewer.yesText().c_str());
        check(k.viewer.question().find("Replace GAMES.DAT in TFDRIVE with SPACE.LIF") != std::string::npos &&
              k.viewer.yesText() == "Replace", "a cassette in: \"Replace GAMES.DAT in TFDRIVE with SPACE.LIF?\"");
        k.viewer.answer(true, k.t);
        for (int i = 0; i < 300 && k.viewer.busy(); ++i) k.step();     // 3 s: the drive still busy
        std::printf("     %s\n", k.viewer.status().c_str());
        check(k.viewer.busy() && k.drive->media == "GAMES.DAT" && k.drive->opens == 0,
              "the drive busy: HIPI waits, the old cassette stays in (an HP-IL command is never cut)");
        check(k.viewer.status().find("waiting for TFDRIVE") != std::string::npos, "... and says so");
        k.drive->isQuiet = true;
        k.runUntilSettled(5);
        check(k.viewer.step() == Uploader::Step::Done && k.drive->media == "SPACE.LIF" && readFile("lif/SPACE.LIF") == vol,
              "quiet: SPACE.LIF goes in");
        check(std::filesystem::exists("lif/GAMES.DAT"), "GAMES.DAT stays on the SD card");
    }

    // ── The same name as on the SD card: overwritten (said beforehand) ───
    {
        Link k;
        k.drive->media = "GAMES.DAT";
        const std::vector<std::uint8_t> vol = lifVolume(32, 0x77);
        std::string why;
        k.viewer.startWith("GAMES.DAT", vol, k.t, why);
        k.runUntilSettled(3);
        std::printf("     \"%s\"\n", k.viewer.question().c_str());
        check(k.viewer.question().find("overwritten") != std::string::npos, "GAMES.DAT again: the question says it's overwritten");
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        check(k.viewer.step() == Uploader::Step::Done && readFile("lif/GAMES.DAT") == vol && k.drive->media == "GAMES.DAT",
              "... replaced, in the drive");
    }

    // ── Cancelled: nothing changes ───────────────────────────────────────
    {
        Link k;
        k.drive->media = "GAMES.DAT";
        std::string why;
        k.viewer.startWith("OTHER.DAT", lifVolume(8, 1), k.t, why);
        k.runUntilSettled(3);
        k.viewer.answer(false, k.t);
        k.runUntilSettled(2);
        check(k.viewer.step() == Uploader::Step::Failed && !std::filesystem::exists("lif/OTHER.DAT") &&
              k.drive->media == "GAMES.DAT" && k.drive->opens == 0, "Cancel: nothing written, the cassette as it was");
    }

    // ── Refused by HIPI ──────────────────────────────────────────────────
    {
        auto refused = [](Link& k, const std::string& name, const char* what) {
            std::string why;
            k.viewer.startWith(name, lifVolume(8, 1), k.t, why);
            k.runUntilSettled(3);
            std::printf("     %s\n", k.viewer.status().c_str());
            check(k.viewer.step() == Uploader::Step::Failed, what);
        };
        { Link k; k.tapeShown = false; refused(k, "A.DAT", "another view than the Tape view: refused (show the Tape view)"); }
        { Link k; k.notNow = "the SD card is lent to the PC (Connect to PC)"; refused(k, "A.DAT", "the SD card lent to the PC: refused"); }
        { Link k; refused(k, "INDRIVE2.DAT", "a file another drive has in it: refused"); }
    }

    // ── A lossy link: pieces and answers lost, still whole ───────────────
    {
        Link k;
        k.loss = 0.2;
        std::vector<std::uint8_t> vol = lifVolume(200, 0x11);
        for (std::size_t i = 256; i < vol.size(); ++i) vol[i] = static_cast<std::uint8_t>(i * 7);
        std::string why;
        k.viewer.startWith("LOSSY.DAT", vol, k.t, why);
        for (int i = 0; i < 3000 && k.viewer.step() != Uploader::Step::Confirm && k.viewer.busy(); ++i) k.step();
        k.viewer.answer(true, k.t);
        for (int i = 0; i < 20000 && k.viewer.busy(); ++i) k.step();
        std::printf("     %s  (%.1f s, %llu bytes sent for %zu)\n", k.viewer.status().c_str(), k.t,
                    static_cast<unsigned long long>(k.bytesToHipi), vol.size());
        check(k.viewer.step() == Uploader::Step::Done && readFile("lif/LOSSY.DAT") == vol,
              "20 % of the messages lost each way: still arrives whole");
    }

    // ── Damaged on the way: the CRC-32 catches it, nothing changes ───────
    {
        Link k;
        k.drive->media = "GAMES.DAT";
        k.damageData = 1;
        std::string why;
        k.viewer.startWith("DAMAGED.DAT", lifVolume(16, 2), k.t, why);
        k.runUntilSettled(3);
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        std::printf("     %s\n", k.viewer.status().c_str());
        check(k.viewer.step() == Uploader::Step::Failed && k.viewer.status().find("CRC") != std::string::npos &&
              !std::filesystem::exists("lif/DAMAGED.DAT") && !std::filesystem::exists(hipi::UploadReceiver::kTempFile) &&
              k.drive->media == "GAMES.DAT", "a damaged piece: refused (CRC), nothing changed");
    }

    // ── A drive that stays busy: given up, the cassette as it was ────────
    {
        Link k;
        k.drive->media = "GAMES.DAT";
        k.drive->isQuiet = false;
        std::string why;
        k.viewer.startWith("LATE.DAT", lifVolume(16, 3), k.t, why);
        k.runUntilSettled(3);
        k.viewer.answer(true, k.t);
        for (int i = 0; i < 4000 && k.viewer.busy(); ++i) k.step();     // 40 s
        std::printf("     %s\n", k.viewer.status().c_str());
        check(k.viewer.step() == Uploader::Step::Failed && k.drive->media == "GAMES.DAT" &&
              !std::filesystem::exists("lif/LATE.DAT") && !std::filesystem::exists(hipi::UploadReceiver::kTempFile),
              "the drive busy for 30 s: given up, nothing changed");
    }

    // ── The viewer gone half-way: dropped ────────────────────────────────
    {
        Link k;
        std::string why;
        k.viewer.startWith("HALF.DAT", lifVolume(400, 4), k.t, why);
        k.runUntilSettled(3);
        k.viewer.answer(true, k.t);
        for (int i = 0; i < 3; ++i) k.step();
        check(k.hipiSide.receiving() && std::filesystem::exists(hipi::UploadReceiver::kTempFile), "half-way ...");
        k.hipiSide.viewerGone();
        check(!k.hipiSide.receiving() && !std::filesystem::exists(hipi::UploadReceiver::kTempFile) &&
              !std::filesystem::exists("lif/HALF.DAT"), "... the viewer gone: dropped, nothing left behind");
    }

    if (chdir(cwd) != 0) return 1;
    std::filesystem::remove_all(sd);
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
