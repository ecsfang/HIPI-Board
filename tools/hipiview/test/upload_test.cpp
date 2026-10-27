// upload_test.cpp -- a LIF volume dropped on hipiview's Tape view, sent to
// HIPI: hipiview's side (upload.hpp) against HIPI's (src/mirror_upload.cpp)
// in one process, the "SD card" a folder, the drive a stand-in. Messages
// can be lost or damaged on the way, the drive can be busy.
//
//   make upload_test && ./upload_test   (in tools/hipiview)
#include "../upload.hpp"
#include "mirror_upload.h"
#include "lif_dir.h"
#include "lif_create.h"

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
    // (its cassette from the "SD card", as the drive's own file would read)
    bool readBlock(std::uint32_t block, std::uint8_t* out) override {
        std::ifstream f("lif/" + media, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(block) * 256);
        f.read(reinterpret_cast<char*>(out), 256);
        return f.gcount() == 256;
    }
    std::uint32_t fileSize() const override {
        std::error_code ec;
        const auto n = std::filesystem::file_size("lif/" + media, ec);
        return ec || media.empty() ? 0 : static_cast<std::uint32_t>(n);
    }
};

// A LIF volume's directory, read from the "SD card"
hipi::LifDir dirOf(const std::string& file) {
    hipi::LifDir d;
    std::string why;
    const std::string path = "lif/" + file;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    hipi::lifReadDir([&](std::uint32_t b, std::uint8_t* out) {
        std::ifstream f(path, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(b) * 256);
        f.read(reinterpret_cast<char*>(out), 256);
        return f.gcount() == 256;
    }, ec ? 0 : static_cast<std::uint32_t>(size / 256), d, why);
    return d;
}
// A file's blocks on a cassette
std::vector<std::uint8_t> blocksOf(const std::string& file, const hipi::LifDirEntry& e) {
    const std::vector<std::uint8_t> all = [&] {
        std::ifstream f("lif/" + file, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }();
    return std::vector<std::uint8_t>(all.begin() + e.start * 256, all.begin() + (e.start + e.blocks) * 256);
}
// An empty cassette, as Create media makes it
void newCassette(const std::string& name, int dirEntries = 64) {
    hipi::LifCreator c;
    c.begin(name, hipi::kLifMediaTypes[0], dirEntries, nullptr);
    while (c.step() == hipi::LifCreator::State::Running) {}
}

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

    // ── A single file: an HP-41 program (.raw) added to the cassette ─────
    {
        const std::vector<std::uint8_t> raw = readFile(std::string(cwd) + "/test/data/vectop.raw");
        hipi::LifFileImage ref;
        std::string why;
        check(raw.size() == 388 && hipi::lifFileFromPc("vectop.raw", raw, ref, why) && ref.name == "VECTOP" &&
              ref.type == 0xE080 && ref.blocks() == 2, "vectop.raw: an HP-41 program, VECTOP, PGM41, 2 blocks");
        check(ref.bytes[28] == 0x01 && ref.bytes[29] == 0x84 && ref.bytes[31] == 0x20 &&
              ref.bytes[32 + 388] == [&] { unsigned s = 0; for (auto b : raw) s += b; return static_cast<std::uint8_t>(s); }(),
              "... its length (388) in the entry, the checksum after the program (as lifutils' raw41lif)");

        newCassette("T1");
        Link k;
        k.drive->media = "T1.DAT";
        check(k.viewer.startWith("vectop.raw", raw, k.t, why), "vectop.raw dropped: converted, HIPI asked");
        k.runUntilSettled(3);
        std::printf("     \"%s\" [%s]\n", k.viewer.question().c_str(), k.viewer.yesText().c_str());
        check(k.viewer.step() == Uploader::Step::Confirm &&
              k.viewer.question() == "Add VECTOP (HP-41 program, vectop.raw) to T1.DAT in TFDRIVE?" &&
              k.viewer.yesText() == "Add", "\"Add VECTOP (HP-41 program, vectop.raw) to T1.DAT in TFDRIVE?\" [Add]");
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        std::printf("     %s\n", k.viewer.status().c_str());
        hipi::LifDir d = dirOf("T1.DAT");
        const hipi::LifDirEntry* e = d.find("VECTOP");
        check(k.viewer.step() == Uploader::Step::Done && k.viewer.status() == "VECTOP is on T1.DAT in TFDRIVE now",
              "done: \"VECTOP is on T1.DAT in TFDRIVE now\"");
        check(e != nullptr && e->type == 0xE080 && e->blocks == 2 && e->start == 2 + 8 && d.files.size() == 1,
              "T1.DAT: VECTOP in its directory, PGM41, 2 blocks, right after the directory");
        check(e != nullptr && blocksOf("T1.DAT", *e) == std::vector<std::uint8_t>(ref.bytes.begin() + 32, ref.bytes.end()),
              "... its blocks are the program, byte for byte");
        check(d.firstFree == 1 && d.freeIsEnd, "... the end mark one on");
        check(k.drive->opens == 1 && k.drive->inserts == 1 && k.drive->media == "T1.DAT",
              "TFDRIVE: the cassette out and in again (as OPEN), still T1.DAT");
        check(!std::filesystem::exists(hipi::UploadReceiver::kTempFile), "the temporary file is gone");

        // Again: there already -- Skip leaves it
        Link k2;
        k2.drive->media = "T1.DAT";
        std::vector<std::uint8_t> raw2 = raw;
        raw2[5] = 'Q';                                                // LBL "SQH"
        k2.viewer.startWith("VECTOP.RAW", raw2, k2.t, why);
        k2.runUntilSettled(3);
        std::printf("     \"%s\" [%s] [%s]\n", k2.viewer.question().c_str(), k2.viewer.noText().c_str(), k2.viewer.yesText().c_str());
        check(k2.viewer.question().find("T1.DAT in TFDRIVE has a file VECTOP already") != std::string::npos &&
              k2.viewer.yesText() == "Replace" && k2.viewer.noText() == "Skip", "there already: \"... Replace it?\" [Skip] [Replace]");
        k2.viewer.answer(false, k2.t);
        k2.runUntilSettled(2);
        check(k2.viewer.status().rfind("skipped", 0) == 0 && blocksOf("T1.DAT", *dirOf("T1.DAT").find("VECTOP"))[5] == 'P',
              "Skip: nothing changed");
        // ... Replace puts the new one in its place
        Link k3;
        k3.drive->media = "T1.DAT";
        k3.viewer.startWith("VECTOP.RAW", raw2, k3.t, why);
        k3.runUntilSettled(3);
        k3.viewer.answer(true, k3.t);
        k3.runUntilSettled(10);
        d = dirOf("T1.DAT");
        check(k3.viewer.step() == Uploader::Step::Done && d.files.size() == 1 && d.find("VECTOP") != nullptr &&
              blocksOf("T1.DAT", *d.find("VECTOP"))[5] == 'Q', "Replace: the new VECTOP in the old one's place, still one file");

        // Another program: a second entry, its blocks after the first
        Link k4;
        k4.drive->media = "T1.DAT";
        check(k4.viewer.startWith("a very long name, 2.raw", raw, k4.t, why), "\"a very long name, 2.raw\" dropped");
        k4.runUntilSettled(3);
        k4.viewer.answer(true, k4.t);
        k4.runUntilSettled(10);
        d = dirOf("T1.DAT");
        const hipi::LifDirEntry* e2 = d.find("AVERYLONGN");
        check(e2 != nullptr && d.files.size() == 2 && e2->index == 1 && e2->start == 12 && d.firstFree == 2,
              "... named AVERYLONGN (letters and digits, 10 at most), entry 2, blocks 12-13");

        // Refused on the PC: no END in it; an extension HIPI doesn't know
        Link k5;
        std::vector<std::uint8_t> noEnd(raw.begin(), raw.end() - 3);
        check(!k5.viewer.startWith("HALF.RAW", noEnd, k5.t, why) && why.find("END") != std::string::npos,
              "a .raw without END: refused on the PC");
        check(!k5.viewer.startWith("NOTES.TXT", raw, k5.t, why) && why.find(".raw") != std::string::npos,
              "a .txt: refused (the message names .raw too)");
    }
    // ── A file with its directory entry (lifget's NAME.lif, a Save on PC):
    // added as it is -- its own name (HP-41 style, "41CV-AL") and type
    {
        newCassette("T2");
        std::vector<std::uint8_t> lif(32, 0);
        const char* n = "41CV-AL   ";
        std::copy(n, n + 10, lif.begin());
        lif[10] = 0xE0; lif[11] = 0x40; lif[15] = 9; lif[19] = 11; lif[26] = 0x80; lif[27] = 0x01;
        lif[28] = 0x01; lif[29] = 0x50; lif[31] = 0x20;
        for (int i = 0; i < 2689; ++i) lif.push_back(static_cast<std::uint8_t>(i * 13));
        Link k;
        k.drive->media = "T2.DAT";
        std::string why;
        check(k.viewer.startWith("41CV-AL.lif", lif, k.t, why), "41CV-AL.lif (an ALL41, its entry first) dropped");
        k.runUntilSettled(3);
        std::printf("     \"%s\" [%s]\n", k.viewer.question().c_str(), k.viewer.yesText().c_str());
        check(k.viewer.question() == "Add 41CV-AL (HP-41 write all, 41CV-AL.lif) to T2.DAT in TFDRIVE?",
              "\"Add 41CV-AL (HP-41 write all, 41CV-AL.lif) to T2.DAT in TFDRIVE?\"");
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        const hipi::LifDir d = dirOf("T2.DAT");
        const hipi::LifDirEntry* e = d.find("41CV-AL");
        check(k.viewer.step() == Uploader::Step::Done && e != nullptr && e->type == 0xE040 && e->blocks == 11 &&
              e->start == 10 && e->entry[28] == 0x01 && e->entry[29] == 0x50 && e->entry[31] == 0x20,
              "... on T2.DAT: 41CV-AL, ALL41, 11 blocks, its entry's own bytes (336 registers)");
        check(e != nullptr && [&] { const auto got = blocksOf("T2.DAT", *e); return std::equal(lif.begin() + 32, lif.end(), got.begin()); }(),
              "... its data as it was");
        if (e == nullptr) std::printf("     (%s)\n", k.viewer.status().c_str());
    }
    // ── No cassette: a new one made for it ──────────────────────────────
    {
        const std::vector<std::uint8_t> raw = readFile(std::string(cwd) + "/test/data/vectop.raw");
        Link k;
        std::string why;
        k.viewer.startWith("vectop.raw", raw, k.t, why);
        k.runUntilSettled(3);
        std::printf("     \"%s\" [%s]\n", k.viewer.question().c_str(), k.viewer.yesText().c_str());
        check(k.viewer.question().find("TFDRIVE is empty") != std::string::npos &&
              k.viewer.question().find("a new cassette, CASS01.DAT") != std::string::npos,
              "the drive empty: \"... on a new cassette, CASS01.DAT?\"");
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        const hipi::LifDir d = dirOf("CASS01.DAT");
        check(k.viewer.step() == Uploader::Step::Done && k.drive->media == "CASS01.DAT" && d.mediumBlocks == 512 &&
              d.dirBlocks == 8 && d.find("VECTOP") != nullptr, "CASS01.DAT made (512 blocks, 64 entries), VECTOP on it, in TFDRIVE");
    }
    // ── A short image (it ends after its last file): the header's size counts
    {
        const std::vector<std::uint8_t> raw = readFile(std::string(cwd) + "/test/data/vectop.raw");
        newCassette("SHORT");
        std::filesystem::resize_file("lif/SHORT.DAT", 10 * 256);       // header, directory -- no data blocks
        Link k;
        k.drive->media = "SHORT.DAT";
        std::string why;
        k.viewer.startWith("vectop.raw", raw, k.t, why);
        k.runUntilSettled(3);
        check(k.viewer.step() == Uploader::Step::Confirm, "SHORT.DAT (10 blocks of 512 in the file): room enough, asked");
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        const hipi::LifDir d = dirOf("SHORT.DAT");
        check(k.viewer.step() == Uploader::Step::Done && d.mediumBlocks == 512 && d.find("VECTOP") != nullptr &&
              d.find("VECTOP")->start == 10 && std::filesystem::file_size("lif/SHORT.DAT") == 12 * 256,
              "... VECTOP added at block 10, the file 12 blocks long now");
    }
    // ── A cassette the HP-41 made (NEWM 40): no medium size in its header,
    // the file only as long as written -- a cassette, 512 blocks
    {
        const std::vector<std::uint8_t> raw = readFile(std::string(cwd) + "/test/data/vectop.raw");
        std::vector<std::uint8_t> img(7 * 256, 0);
        img[0] = 0x80; img[11] = 2; img[12] = 0x10; img[19] = 5;   // directory at 2, 5 blocks; tracks etc. 0
        std::fill(img.begin() + 2 * 256, img.end(), 0xFF);
        { std::ofstream("lif/NEWM.DAT", std::ios::binary).write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size())); }
        Link k;
        k.drive->media = "NEWM.DAT";
        std::string why;
        k.viewer.startWith("vectop.raw", raw, k.t, why);
        k.runUntilSettled(3);
        k.viewer.answer(true, k.t);
        k.runUntilSettled(10);
        const hipi::LifDir d = dirOf("NEWM.DAT");
        check(k.viewer.step() == Uploader::Step::Done && d.mediumBlocks == 512 && d.find("VECTOP") != nullptr &&
              d.find("VECTOP")->start == 7, "a cassette from the HP-41's NEWM (no size in its header, 7 blocks long): VECTOP added at block 7");
    }
    // ── No room: said when asked, nothing sent ──────────────────────────
    {
        const std::vector<std::uint8_t> raw = readFile(std::string(cwd) + "/test/data/vectop.raw");
        newCassette("FULL", 8);
        // 8 files fill its directory
        hipi::LifFileImage img;
        std::string why;
        hipi::lifFileFromPc("x.raw", raw, img, why);
        for (int i = 0; i < 8; ++i) {
            img.bytes[1] = static_cast<std::uint8_t>('A' + i);       // XA, XB ...
            std::fstream f("lif/FULL.DAT", std::ios::binary | std::ios::in | std::ios::out);
            auto rd = [&](std::uint32_t b, std::uint8_t* out) {
                f.seekg(static_cast<std::streamoff>(b) * 256); f.read(reinterpret_cast<char*>(out), 256); return f.gcount() == 256;
            };
            auto wr = [&](std::uint32_t b, const std::uint8_t* in) {
                f.seekp(static_cast<std::streamoff>(b) * 256); f.write(reinterpret_cast<const char*>(in), 256); return f.good();
            };
            hipi::lifAddFile(rd, wr, 512, img.bytes.data(),
                             [&](std::uint32_t b, std::uint8_t* out) { std::memcpy(out, img.bytes.data() + 32 + b * 256, 256); return true; },
                             false, why);
        }
        check(dirOf("FULL.DAT").files.size() == 8 && dirOf("FULL.DAT").firstFree < 0, "FULL.DAT: 8 files, its directory full");
        Link k;
        k.drive->media = "FULL.DAT";
        k.viewer.startWith("vectop.raw", raw, k.t, why);
        k.runUntilSettled(3);
        check(k.viewer.step() == Uploader::Step::Failed && k.viewer.status().find("directory is full") != std::string::npos,
              "a ninth: \"HIPI: FULL.DAT: the directory is full\" -- before anything is sent");
    }

    if (chdir(cwd) != 0) return 1;
    std::filesystem::remove_all(sd);
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
