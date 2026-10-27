// download_test.cpp -- a drive's cassette saved on the PC: HIPI's side
// (src/mirror_download.cpp) against hipiview's (download.hpp) in one
// process, the drive a stand-in with its file in memory. The stream is
// reliable (display_mirror.cpp), so nothing is lost here -- but the ring can
// be full, the drive busy or written to meanwhile, the viewer gone.
//
//   make download_test && ./download_test   (in tools/hipiview)
#include "../download.hpp"
#include "mirror_download.h"
#include "lif_dir.h"
#include "lif_file.h"
#include <fstream>
#include <iterator>

#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

std::uint64_t nowMs = 1000;

// The drive: what HIPI's would do (src/pico_main.cpp's DriveForDownload)
struct FakeDrive : hipi::DownloadDrive {
    std::string nm = "TFDRIVE", media = "GAMES.DAT";
    std::vector<std::uint8_t> file;
    bool isQuiet = true;
    std::uint32_t writes = 0;
    int reads = 0;
    std::string name() const override { return nm; }
    std::string mediaFile() const override { return media; }
    bool quiet() const override { return isQuiet; }
    std::uint32_t changes() const override { return writes; }
    std::uint32_t fileSize() const override { return media.empty() ? 0 : static_cast<std::uint32_t>(file.size()); }
    bool readBlock(std::uint32_t block, std::uint8_t* out) override {
        ++reads;
        for (std::uint32_t i = 0; i < 256; ++i) {
            const std::size_t at = block * 256u + i;
            out[i] = at < file.size() ? file[at] : 0xFF;     // (CTapeSD pads with 255)
        }
        return true;
    }
    // The HP-41 writes a block
    void write(std::uint32_t block, std::uint8_t v) {
        for (std::uint32_t i = 0; i < 256 && block * 256u + i < file.size(); ++i) file[block * 256u + i] = v;
        ++writes;
    }
};

std::vector<std::uint8_t> pattern(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 2654435761u + seed) >> 13);
    return v;
}

// Both ends wired together. room: how many pieces the "ring" takes per
// poll (-1: any number)
struct Link {
    std::shared_ptr<FakeDrive> drive = std::make_shared<FakeDrive>();
    bool tapeShown = true;
    std::string notNow;
    int room = -1, roomLeft = 0;
    std::deque<std::vector<std::uint8_t>> toHipi;
    std::deque<std::pair<std::uint8_t, std::vector<std::uint8_t>>> toViewer;
    hipi::MediaSender hipiSide;
    Downloader viewer;
    double t = 0;
    int pieces = 0;

    Link()
        : hipiSide(env(),
                   [this](std::uint8_t k, const std::uint8_t* p, std::size_t n) {
                       if (k == hipi::mirror::kDlData) ++pieces;
                       toViewer.emplace_back(k, std::vector<std::uint8_t>(p, p + n));
                   },
                   [this](std::size_t) { return room < 0 || roomLeft-- > 0; }),
          viewer([this](std::vector<std::uint8_t> m) { toHipi.push_back(std::move(m)); }) {
        drive->file = pattern(64 * 1024 + 300, 1);         // (not a whole number of blocks)
    }

    hipi::DownloadEnv env() {
        hipi::DownloadEnv e;
        e.drive = [this]() -> std::shared_ptr<hipi::DownloadDrive> { return tapeShown ? drive : nullptr; };
        e.notNow = [this] { return notNow; };
        e.nowMs = [] { return nowMs; };
        return e;
    }

    // 10 ms: HIPI's main loop (the viewer's messages, the sender), then the
    // viewer's frame (what came, its clocks)
    void step() {
        while (!toHipi.empty()) {
            const std::vector<std::uint8_t> m = toHipi.front();
            toHipi.pop_front();
            if (m.size() < 7 || m[0] != hipi::mirror::kMsgFile) continue;
            if (m[2] == hipi::mirror::kDlAsk) hipiSide.ask();
            else if (m[2] == hipi::mirror::kDlAbort) hipiSide.abort();
        }
        roomLeft = room;
        hipiSide.poll();
        while (!toViewer.empty()) {
            auto r = toViewer.front();
            toViewer.pop_front();
            viewer.onPacket(r.first, r.second.data(), r.second.size(), t);
        }
        nowMs += 10;
        t += 0.01;
        viewer.tick(t);
    }
    // Until the viewer has it, or gave up (or `seconds` went by)
    void run(double seconds, const std::function<void()>& each = nullptr) {
        for (double end = t + seconds; t < end; ) {
            step();
            if (each) each();
            if (viewer.step() == Downloader::Step::Ready || viewer.step() == Downloader::Step::Failed) return;
        }
    }
    bool got(std::vector<std::uint8_t>& data, std::string& name, std::string& drv) {
        return viewer.take(name, drv, data);
    }
};

}  // namespace

int main() {
    {
        // M in hipiview: asked, sent, the same bytes
        Link l;
        l.viewer.ask(l.t);
        l.run(10);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv), "M: the cassette arrived");
        check(d == l.drive->file, "... the same bytes (64 kB + 300: the last block cut to the file's size)");
        check(name == "GAMES.DAT" && drv == "TFDRIVE", "... its name and drive");
        check(l.viewer.askedHere(), "... asked for here");
        check(l.viewer.step() == Downloader::Step::Idle, "... taken: idle again");
        check(!l.hipiSide.busy(), "HIPI's side: idle again");
    }
    {
        // From HIPI's menu: it just arrives
        Link l;
        std::string why;
        check(l.hipiSide.start(why), "HIPI's menu: Save media on PC");
        l.run(10);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file, "... arrives without being asked");
        check(!l.viewer.askedHere(), "... not asked for here");
    }
    {
        // A second one while the first is on its way: refused, the first goes on
        Link l;
        l.room = 1;
        std::string why;
        l.hipiSide.start(why);
        l.step();
        check(!l.hipiSide.start(why) && why.find("on its way") != std::string::npos, "a second one meanwhile: refused (\"on its way\")");
        l.run(30);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file, "... the first one arrives whole");
    }
    {
        // The ring full now and then: pieces wait, nothing is lost
        Link l;
        l.room = 0;
        l.viewer.ask(l.t);
        l.run(1);
        check(l.viewer.step() == Downloader::Step::Receiving && l.pieces == 0, "ring full: begun, no piece yet");
        l.room = 2;
        l.run(10);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file, "... then two pieces a poll: arrives whole");
    }
    {
        // The drive busy (an HP-41 command): waits, says so, then sends
        Link l;
        l.drive->isQuiet = false;
        l.viewer.ask(l.t);
        l.run(3);
        check(l.viewer.step() == Downloader::Step::Asking && l.drive->reads == 0, "drive busy: nothing read meanwhile");
        check(l.viewer.status().find("waiting for TFDRIVE") != std::string::npos, "... the viewer is told it waits");
        l.drive->isQuiet = true;
        l.run(10);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file, "... quiet again: arrives");
    }
    {
        // Busy halfway: it pauses, and goes on where it was
        Link l;
        l.room = 4;
        l.viewer.ask(l.t);
        l.run(0.05);
        const int before = l.drive->reads;
        l.drive->isQuiet = false;
        l.run(1);
        check(l.drive->reads == before, "busy halfway: no reading while busy");
        l.drive->isQuiet = true;
        l.run(10);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file && l.viewer.restarts() == 0, "... then on where it was: whole, no restart");
    }
    {
        // Written to halfway: from the start again -- the cassette as it is now
        Link l;
        l.room = 4;
        l.viewer.ask(l.t);
        l.run(0.05);
        l.drive->write(1, 0x55);                       // (a block already sent)
        l.run(10);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file, "written to halfway: arrives as it is now");
        check(d[256] == 0x55 && l.viewer.restarts() == 1, "... the new block in it, started over once");
    }
    {
        // Written to all the time: given up
        Link l;
        l.room = 1;
        l.viewer.ask(l.t);
        int n = 0;
        l.run(30, [&] { if (++n % 20 == 0) l.drive->write(0, static_cast<std::uint8_t>(n)); });
        check(l.viewer.step() == Downloader::Step::Failed &&
              l.viewer.status().find("keeps writing") != std::string::npos, "written to all the time: given up (\"keeps writing\")");
        check(!l.hipiSide.busy(), "... HIPI's side idle");
    }
    {
        // Busy for ever: given up after 30 s
        Link l;
        l.drive->isQuiet = false;
        l.viewer.ask(l.t);
        l.run(40);
        check(l.viewer.step() == Downloader::Step::Failed &&
              l.viewer.status().find("stays busy") != std::string::npos, "busy for ever: given up (\"stays busy\")");
    }
    {
        // The cassette changed meanwhile (the file picker)
        Link l;
        l.room = 1;
        l.viewer.ask(l.t);
        l.run(0.1);
        l.drive->media = "OTHER.DAT";
        l.run(5);
        check(l.viewer.step() == Downloader::Step::Failed &&
              l.viewer.status().find("was changed") != std::string::npos, "the cassette changed meanwhile: an error");
    }
    {
        // Refusals: told to the viewer, which asked
        Link l;
        l.tapeShown = false;
        l.viewer.ask(l.t);
        l.run(2);
        check(l.viewer.step() == Downloader::Step::Failed && l.viewer.status().find("Tape view") != std::string::npos,
              "another view shown: \"show the Tape view\"");
        Link m;
        m.drive->media.clear();
        m.viewer.ask(m.t);
        m.run(2);
        check(m.viewer.step() == Downloader::Step::Failed && m.viewer.status().find("no cassette") != std::string::npos,
              "no media: \"has no cassette\"");
        Link k;
        k.notNow = "the SD card is lent to the PC (Connect to PC)";
        k.viewer.ask(k.t);
        k.run(2);
        check(k.viewer.step() == Downloader::Step::Failed && k.viewer.status().find("lent") != std::string::npos,
              "SD card lent to the PC: refused");
        check(k.viewer.askedHere(), "... a message box (asked for here)");
    }
    {
        // HIPI doesn't answer (older firmware): given up after 5 s
        Link l;
        l.viewer.ask(l.t);
        for (int i = 0; i < 700; ++i) {               // (HIPI never reads)
            l.toHipi.clear();
            l.t += 0.01;
            l.viewer.tick(l.t);
        }
        check(l.viewer.step() == Downloader::Step::Failed &&
              l.viewer.status().find("1.6.0") != std::string::npos, "no answer: given up (\"1.6.0 or newer\")");
    }
    {
        // The viewer gone halfway / a new session: both sides stop
        Link l;
        l.room = 1;
        l.viewer.ask(l.t);
        l.run(0.1);
        l.hipiSide.viewerGone();
        l.viewer.onNewSession();
        check(!l.hipiSide.busy(), "a new session halfway: HIPI's side stops");
        check(l.viewer.step() == Downloader::Step::Failed &&
              l.viewer.status().find("started over") != std::string::npos, "... the viewer says so");
    }
    {
        // Cancelled from the viewer
        Link l;
        l.room = 1;
        l.viewer.ask(l.t);
        l.run(0.1);
        l.viewer.cancel();
        l.step();
        check(!l.hipiSide.busy() && l.viewer.step() == Downloader::Step::Idle, "cancelled: both sides idle");
    }
    {
        // A whole HDRIVE-sized volume (2 MB) gets there too
        Link l;
        l.drive->file = pattern(2 * 1024 * 1024, 7);
        l.viewer.ask(l.t);
        l.run(60);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv) && d == l.drive->file, "2 MB: arrives whole");
    }
    // ── One file of the cassette (the Media map's Save on PC) ─────────────
    {
        // A cassette with VECTOP and SECOND on it (as hipiview would add them)
        std::ifstream rf("test/data/vectop.raw", std::ios::binary);
        const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
        std::vector<std::uint8_t> cass(512 * 256, 0);
        cass[0] = 0x80; cass[11] = 2; cass[19] = 8;          // directory at 2, 8 blocks (no medium size: a cassette)
        std::fill(cass.begin() + 2 * 256, cass.begin() + 10 * 256, 0xFF);
        auto add = [&](const char* pcName, const std::vector<std::uint8_t>& in) {
            hipi::LifFileImage img;
            std::string why;
            hipi::lifFileFromPc(pcName, in, img, why);
            hipi::lifAddFile([&](std::uint32_t b, std::uint8_t* o) { std::copy_n(cass.begin() + b * 256, 256, o); return true; },
                             [&](std::uint32_t b, const std::uint8_t* d) { std::copy_n(d, 256, cass.begin() + b * 256); return true; },
                             512, img.bytes.data(),
                             [&](std::uint32_t b, std::uint8_t* o) { std::copy_n(img.bytes.begin() + 32 + b * 256, 256, o); return true; },
                             false, why);
        };
        std::vector<std::uint8_t> raw2 = raw;
        raw2[5] = 'Q';
        add("vectop.raw", raw);
        add("second.raw", raw2);
        // ... and a write-all (ALL41, E040): 336 registers, 2689 bytes in 11 blocks
        std::vector<std::uint8_t> all(32 + 11 * 256, 0);
        {
            const char* n = "WALL      ";
            std::copy(n, n + 10, all.begin());
            all[10] = 0xE0; all[11] = 0x40; all[19] = 11; all[26] = 0x80; all[27] = 0x01;
            all[28] = 0x01; all[29] = 0x50; all[31] = 0x20;
            for (std::size_t i = 32; i < all.size(); ++i) all[i] = static_cast<std::uint8_t>(i * 7);
            std::string why;
            hipi::lifAddFile([&](std::uint32_t b, std::uint8_t* o) { std::copy_n(cass.begin() + b * 256, 256, o); return true; },
                             [&](std::uint32_t b, const std::uint8_t* d) { std::copy_n(d, 256, cass.begin() + b * 256); return true; },
                             512, all.data(),
                             [&](std::uint32_t b, std::uint8_t* o) { std::copy_n(all.begin() + 32 + b * 256, 256, o); return true; },
                             false, why);
        }
        // What lifutils' lifget -r gives: the program and its checksum
        std::vector<std::uint8_t> want = raw2;
        unsigned sum = 0;
        for (auto b : raw2) sum += b;
        want.push_back(static_cast<std::uint8_t>(sum));

        Link l;
        l.drive->file = cass;
        l.drive->media = "TAPE1.DAT";
        std::string why;
        check(l.hipiSide.startFile("SECOND", why), "Save on PC: SECOND on TAPE1.DAT");
        l.run(5);
        std::vector<std::uint8_t> d;
        std::string name, drv;
        check(l.got(d, name, drv), "... arrives");
        check(name == "SECOND.raw" && drv == "TAPE1.DAT in TFDRIVE", "... as SECOND.raw, from TAPE1.DAT in TFDRIVE");
        check(d == want, "... 389 bytes: the program and its checksum, as lifget -r gives it");
        check(!l.hipiSide.busy(), "... HIPI's side idle again");
        // Not a program: NAME.lif, its directory entry first -- as lifget writes it
        Link w;
        w.drive->file = cass;
        w.drive->media = "TAPE1.DAT";
        check(w.hipiSide.startFile("WALL", why), "Save on PC: WALL (a write-all)");
        w.run(5);
        check(w.got(d, name, drv) && name == "WALL.lif", "... arrives as WALL.lif");
        std::vector<std::uint8_t> wantAll(cass.begin() + 2 * 256 + 2 * 32, cass.begin() + 2 * 256 + 3 * 32);   // its entry (3rd)
        wantAll.insert(wantAll.end(), all.begin() + 32, all.begin() + 32 + 336 * 8 + 1);
        check(d == wantAll, "... its entry, then 2689 bytes (336 registers x 8 + 1): as lifget writes it");
        hipi::LifFileImage back;
        check(hipi::lifIsSingleFile(d) && hipi::lifFileFromLifget(d, back, why) && back.name == "WALL" && back.type == 0xE040 &&
              back.blocks() == 11 && std::equal(back.bytes.begin() + 16, back.bytes.begin() + 32, all.begin() + 16),
              "... and is taken back as it was (a drop on the Tape view): WALL, E040, 11 blocks, its entry's bytes");
        Link m;
        m.drive->file = cass;
        check(!m.hipiSide.startFile("NOSUCH", why) && why.find("isn't on") != std::string::npos,
              "a file that isn't there: \"isn't on\"");
        // Written to meanwhile: from the start again, the file as it is then
        Link k;
        k.drive->file = cass;
        k.room = 0;
        k.hipiSide.startFile("VECTOP", why);
        k.run(0.1);
        k.drive->write(500, 0x11);                         // (a block somewhere else)
        k.room = -1;
        k.run(5);
        check(k.got(d, name, drv) && d.size() == 389 && k.viewer.restarts() == 1, "written meanwhile: started over, arrives whole");
    }
    std::printf(failures ? "%d FAILED\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
