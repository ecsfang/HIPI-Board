// mediamap_test.cpp -- the Media map view on the PC: the real mediamap.cpp
// with the LT7683 driver against the emulated chip, on LIF images made
// here (lif_create.h's header, then made-up directory entries). The drive
// is a stand-in: a head that moves, reads and writes. Saves:
//
//   mediamap_1_cassette.png   a cassette: two rings, reading track 0
//   mediamap_7_programs.png   programs one after the other: their boundaries
//   mediamap_8_save.png       a file's box with Save on PC (hipiview connected)
//   mediamap_9_saved.png      ... tapped: the answer in its place
//   mediamap_2_disk.png       a 16 MB disk: eight rings, writing
//   mediamap_3_file.png       a file tapped
//   mediamap_4_highlight.png  HP-41/71/75 files, programs highlighted
//   mediamap_5_machine.png    ... BY MACHINE, HP-71 highlighted
//   mediamap_6_file71.png     ... a secure, private HP-71 file tapped
//
//   make mediamap_test && ./mediamap_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "mediamap.h"
#include "lif_create.h"
#include "hp82163_font.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

LT7683Emu chip;

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

int failures = 0;
std::vector<std::uint32_t> panel(1024 * 600);

void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng(png, panel.data(), 1024, 600);
    std::printf("     saved %s\n", png);
}
std::uint32_t at(int x, int y) {
    chip.render(panel.data(), 0.0, false);
    return panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF;
}
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void makeChipFont() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else for (int r = 0; r < 16; ++r) f[c][r] = (r == 2 || r == 13) ? 0x7E : (r > 2 && r < 13 ? 0x42 : 0);
    }
    chip.setBuiltinFont(f);
}

// An image in memory: the header and directory entries one after another
struct File { std::string name; std::uint16_t type; std::uint32_t len; };
std::vector<std::uint8_t> image;
std::uint32_t firstFree = 0;
void makeImage(const hipi::LifMediaType& t, int entries, const std::vector<File>& files) {
    image.assign(t.bytes(), 0);
    hipi::lifVolumeHeader(image.data(), t, entries, "TEST", nullptr);
    const int dirBlocks = hipi::lifDirBlocks(entries);
    std::memset(image.data() + 512, 0xFF, static_cast<std::size_t>(dirBlocks) * 256);
    std::uint32_t at = 2 + static_cast<std::uint32_t>(dirBlocks);
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::uint8_t* e = image.data() + 512 + i * 32;
        std::memset(e, ' ', 10);
        std::memcpy(e, files[i].name.data(), std::min<std::size_t>(10, files[i].name.size()));
        e[10] = static_cast<std::uint8_t>(files[i].type >> 8); e[11] = static_cast<std::uint8_t>(files[i].type);
        for (int k = 0; k < 4; ++k) {
            e[12 + k] = static_cast<std::uint8_t>(at >> (24 - 8 * k));
            e[16 + k] = static_cast<std::uint8_t>(files[i].len >> (24 - 8 * k));
        }
        at += files[i].len;
    }
    firstFree = at;
}

// The stand-in drive
hipi::MediaMapLive drive;
bool haveDrive = true;
std::uint32_t nowMs = 1000;
void run(int ms) { for (int t = 0; t < ms; t += 10) { nowMs += 10; hipi::mediamap_poll(nowMs); } }

}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFont();
    hipi::mediamap_init(&d);
    int reads = 0;
    hipi::mediamap_setSource(
        [](hipi::MediaMapLive& l) { l = drive; return haveDrive; },
        [&](std::uint32_t blk, std::uint8_t* b) {
            if (static_cast<std::size_t>(blk + 1) * 256 > image.size()) return false;
            ++reads;
            std::memcpy(b, image.data() + blk * 256, 256);
            return true;
        });

    // ── A cassette ─────────────────────────────────────────────────────
    makeImage(hipi::kLifMediaTypes[0], 64, {
        { "README", 0xE0D5, 4 }, { "KA", 0xE050, 2 }, { "SWAPVE", 0xE080, 53 }, { "OLD1", 0x0000, 40 },
        { "RMCHKX", 0xE080, 33 }, { "DATA1", 0xE0D0, 60 }, { "ST1", 0xE060, 3 }, { "OLD2", 0x0000, 30 },
        { "GAMES", 0xE080, 90 }, { "ROMIMG", 0xE070, 16 }, { "TINY", 0xE080, 1 } });
    drive.file = "GAMES.DAT";
    drive.head = 61; drive.op = hipi::MediaOp::Read;
    hipi::mediamap_enter();
    hipi::mediamap_drawAll();
    snapshot("mediamap_1_cassette.png");
    check(hipi::mediamap_rings() == 2, "cassette: two rings (its tracks)");
    check(hipi::mediamap_blocksOf(2) == 53 + 33 + 90 + 1 && hipi::mediamap_blocksOf(1) == 70,
          "counted from the directory: programs 177 records, deleted 70");
    check(hipi::mediamap_blocksOf(0) == 512 - firstFree, "free: what's after the last file");
    check(reads == 3, "read: the header and the directory up to its end mark (2 of 8 blocks)");
    // The middle: the operation, centred
    {
        const int cx = 20 + 275, cy = 26 + 14 + 275;
        chip.render(panel.data(), 0.0, false);
        int minX = 1024, maxX = 0, minY = 600, maxY = 0;
        for (int x = cx - 110; x < cx + 110; ++x)
            for (int y = cy - 100; y < cy + 100; ++y) {
                const std::uint32_t p = panel[static_cast<std::size_t>(y) * 1024 + x];
                const int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > 100 * 100) continue;          // inside the gold ring
                if ((p & 0xFFFFFF) != 0) { minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y); }
            }
        std::printf("     text in the middle: x %d..%d, y %d..%d (centre %d,%d)\n", minX, maxX, minY, maxY, cx, cy);
        check(std::abs((minX + maxX) / 2 - cx) <= 4 && std::abs((minY + maxY) / 2 - cy) <= 4, "the text in the middle is centred");
    }
    // The head runs along both tracks (DIR, reading): nothing may be left
    // behind of the white line -- only the last one shows
    auto whiteInRings = [] {
        chip.render(panel.data(), 0.0, false);
        const int cx = 20 + 275, cy = 26 + 14 + 275;
        int n = 0;
        for (int y = cy - 280; y < cy + 280; ++y)
            for (int x = cx - 280; x < cx + 280; ++x) {
                const int dx = x - cx, dy = y - cy;
                const int r2 = dx * dx + dy * dy;
                if (r2 < 128 * 128 || r2 > 276 * 276) continue;           // the rings only
                if ((panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF) == 0xFFFFFF) ++n;
            }
        return n;
    };
    const int oneLine = whiteInRings();
    for (std::uint32_t b = 2; b < 512; b += 7) { drive.head = b; run(150); }
    drive.head = 61;
    run(300);
    std::printf("     white pixels in the rings: %d with one head line, %d after the head ran around\n", oneLine, whiteInRings());
    check(whiteInRings() == oneLine, "the head leaves nothing behind (no white dots along the rings)");
    // The head moves
    drive.head = 300; drive.op = hipi::MediaOp::Seek;
    run(300);
    drive.op = hipi::MediaOp::Write; drive.writes = 1; drive.busy = true;
    run(300);
    // ... a new file written: the directory again once the drive is quiet
    std::uint8_t* e = image.data() + 512 + 11 * 32;
    std::memset(e, 0, 32); std::memset(e, ' ', 10); std::memcpy(e, "NEWPRG", 6);
    e[10] = 0xE0; e[11] = 0x80; e[14] = static_cast<std::uint8_t>(firstFree >> 8); e[15] = static_cast<std::uint8_t>(firstFree);
    e[19] = 20;
    run(300);
    check(hipi::mediamap_blocksOf(2) == 177, "busy: not read again yet");
    drive.busy = false;
    run(1000);
    check(hipi::mediamap_blocksOf(2) == 197, "quiet after writing: read again (+20 records of program)");

    // A tap on the outer ring at 12 o'clock + a bit: the first file after the directory
    {
        const int cx = 20 + 275, cy = 26 + 14 + 275;
        const double a = -M_PI / 2 + (12.5 / 256) * 2 * M_PI;      // record 12 of track 0: README
        check(hipi::mediamap_tap(static_cast<int>(cx + 250 * std::cos(a)), static_cast<int>(cy + 250 * std::sin(a))) ==
              hipi::MediaMapTap::Handled, "a tap on a ring: that file's box");
        snapshot("mediamap_3_file.png");
    }
    check(hipi::mediamap_tap(1000, 10) == hipi::MediaMapTap::Close, "the X in the title bar: close");

    // ── Programs one after the other: their boundaries, a darker hair ──
    {
        std::vector<File> progs = { { "SDUMP", 0xE080, 1 }, { "PIXEL", 0xE080, 1 }, { "VECTOP", 0xE080, 2 },
                                    { "GAME1", 0xE080, 12 }, { "GAME2", 0xE080, 20 }, { "ALL", 0xE040, 11 },
                                    { "BIG", 0xE080, 60 }, { "SMALL", 0xE080, 3 } };
        for (int i = 0; i < 24; ++i) progs.push_back({ "T" + std::to_string(i), 0xE080, 1 });   // many tiny ones
        progs.push_back({ "LAST", 0xE080, 40 });
        makeImage(hipi::kLifMediaTypes[0], 64, progs);
        drive.file = "PROGS.DAT";
        drive.op = hipi::MediaOp::None;
        run(300);
        snapshot("mediamap_7_programs.png");
        check(hipi::mediamap_blocksOf(2) == 1 + 1 + 2 + 12 + 20 + 60 + 3 + 24 + 40, "programs after each other: all counted");
        // A file's box: Save on PC (with hipiview there)
        bool viewer = false;
        std::string asked;
        hipi::mediamap_setSaveOnPc([&] { return viewer; },
                                   [&](const std::string& n, std::string& why) { asked = n; why.clear(); return true; });
        int x = 0, y = 0;
        hipi::mediamap_blockPoint(10 + 1 + 1 + 1, x, y);           // VECTOP (after SDUMP, PIXEL)
        hipi::mediamap_tap(x, y);
        check(!hipi::mediamap_savePoint(x, y), "no hipiview: no Save on PC in the file's box");
        viewer = true;
        hipi::mediamap_blockPoint(10 + 1 + 1 + 1, x, y);
        hipi::mediamap_tap(x, y);
        int bx = 0, by = 0;
        check(hipi::mediamap_savePoint(bx, by), "hipiview there: Save on PC in VECTOP's box");
        snapshot("mediamap_8_save.png");
        check(hipi::mediamap_tap(bx, by) == hipi::MediaMapTap::Handled && asked == "VECTOP", "tapped: VECTOP sent (its LIF name)");
        check(!hipi::mediamap_savePoint(bx, by), "... the answer in the button's place");
        snapshot("mediamap_9_saved.png");
        hipi::mediamap_setSaveOnPc(nullptr, nullptr);
    }

    // ── A 16 MB disk ───────────────────────────────────────────────────
    std::mt19937 rng(7);
    std::vector<File> many;
    const std::uint16_t types[] = { 0xE080, 0xE080, 0xE0D0, 0xE0D0, 0xE0D5, 0xE060, 0xE070, 0x0000 };
    for (int i = 0; i < 300; ++i) {
        char n[11];
        std::snprintf(n, sizeof(n), "F%03d", i);
        many.push_back({ n, types[rng() % 8], 1 + static_cast<std::uint32_t>(rng() % 220) });
    }
    makeImage(hipi::kLifMediaTypes[6], 512, many);
    drive.file = "BIGDISK.DAT";
    drive.head = 35214; drive.op = hipi::MediaOp::Write; drive.writes = 5;
    run(300);                                       // another file: drawn afresh
    snapshot("mediamap_2_disk.png");
    check(hipi::mediamap_rings() == 8, "16 MB disk: eight rings");
    {
        const int one = whiteInRings();
        for (std::uint32_t b = 100; b < 64000; b += 997) { drive.head = b; run(150); }
        drive.head = 35214;
        run(300);
        check(whiteInRings() == one, "disk: the head leaves nothing behind either (narrow inner steps)");
    }
    std::uint32_t sum = 0;
    for (int k = 0; k < 10; ++k) sum += hipi::mediamap_blocksOf(k);   // the kinds (not the machines)
    check(sum == 64000, "every block counted once (64000)");

    // Formatting: the header is gone for a while (not LIF), then back
    {
        auto ringsDrawn = [] {                           // the outer ring, around it
            const int cx = 20 + 275, cy = 26 + 14 + 275;
            int n = 0;
            for (int k = 0; k < 36; ++k) {
                const double a = k * M_PI / 18;
                if (at(static_cast<int>(cx + 262 * std::cos(a)), static_cast<int>(cy + 262 * std::sin(a))) != 0) ++n;
            }
            return n > 30;
        };
        check(ringsDrawn(), "the rings are drawn");
        const std::vector<std::uint8_t> good = image;
        std::memset(image.data(), 0, 512);
        drive.op = hipi::MediaOp::Write; drive.writes += 1; drive.busy = true;
        run(300);
        drive.busy = false;
        run(1000);
        check(!ringsDrawn(), "formatting: not LIF for a while");
        bool msg = false;
        for (int x = 300; x < 724 && !msg; ++x) for (int y = 270; y < 310 && !msg; ++y) msg = at(x, y) == 0xFFFFFF;
        check(msg, "... and that's what it says");
        // The header written again
        image = good;
        drive.writes += 1; drive.busy = true;
        run(300);
        drive.busy = false;
        run(1000);
        check(ringsDrawn(), "formatted: the map is back at once (without leaving the view)");
        // Not LIF and nothing written: looked at again anyway, now and then
        std::memset(image.data(), 0, 512);
        drive.writes += 1;
        run(1000);
        image = good;
        run(2500);
        check(ringsDrawn(), "LIF again without a write seen: back within a few seconds");
    }

    // ── HP-41, HP-71 and HP-75 files on one disk ─────────────────────
    {
        makeImage(hipi::kLifMediaTypes[1], 64, {
            { "SECRET", 0xE217, 40 }, { "GAME41", 0xE080, 60 }, { "DATA41", 0xE0D0, 30 }, { "KEYS", 0xE050, 3 },
            { "MATH71", 0xE214, 80 }, { "HPILLX", 0xE208, 50 }, { "NOTES", 0x0001, 20 }, { "CALC75", 0xE088, 45 },
            { "SHEET", 0xE08A, 25 }, { "ODD", 0xE5A5, 12 }, { "GONE", 0x0000, 35 }, { "PLOT71", 0xE222, 15 },
            { "MLDL", 0xE070, 64 }, { "TXT75", 0xE052, 10 } });
        drive.file = "MIXED.DAT";
        drive.head = 20; drive.op = hipi::MediaOp::Read; drive.writes += 1;
        run(300);
        check(hipi::mediamap_highlight() == -1 && !hipi::mediamap_byMachine(), "another medium: BY TYPE, nothing highlighted");
        // Kinds: programs 40+60+80+45, ROM/LEX 50+64, other 15+12 (GRA71, unknown)
        check(hipi::mediamap_blocksOf(2) == 225 && hipi::mediamap_blocksOf(6) == 114 && hipi::mediamap_blocksOf(7) == 27,
              "by type: HP-71 and HP-75 files sorted in (programs 225, ROM/LEX 114, other 27)");
        // Machines: HP-41 60+30+3+64, HP-71 40+80+50+15, HP-75 45+25+10, any 20, unknown 12
        check(hipi::mediamap_blocksOf(10) == 157 && hipi::mediamap_blocksOf(11) == 185 && hipi::mediamap_blocksOf(12) == 80 &&
              hipi::mediamap_blocksOf(13) == 20 && hipi::mediamap_blocksOf(14) == 12,
              "by machine: HP-41 157, HP-71 185, HP-75 80, any 20, unknown 12");
        // The legend's swatch of row i, and how many pixels of a colour the rings have
        const int cx = 20 + 275, cy = 26 + 14 + 275;
        auto swatch = [](int i) { return at(598 + 16 + 6, 26 + 20 + 22 + 8 + i * 26); };
        auto inRings = [&](std::uint32_t col) {
            chip.render(panel.data(), 0.0, false);
            int n = 0;
            for (int y = cy - 276; y < cy + 276; ++y)
                for (int x = cx - 276; x < cx + 276; ++x) {
                    const int dx = x - cx, dy = y - cy;
                    if (dx * dx + dy * dy > 276 * 276) continue;
                    if ((panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF) == col) ++n;
                }
            return n;
        };
        const std::uint32_t progCol = swatch(0), dataCol = swatch(1);
        const int prog = inRings(progCol), data = inRings(dataCol);
        check(prog > 1000 && data > 300, "programs and data in the rings");
        // A tap on "programs": the rest dimmed
        const int rowY = 26 + 20 + 22 + 8;
        check(hipi::mediamap_tap(700, rowY) == hipi::MediaMapTap::Handled && hipi::mediamap_highlight() == 2,
              "a tap on the programs row: programs highlighted");
        snapshot("mediamap_4_highlight.png");
        const std::uint32_t dim = swatch(1);
        std::printf("     prog %d/%d, data %d, dim %d (data before %d)\n", inRings(progCol), prog, inRings(dataCol), inRings(dim), data);
        check(dim != dataCol && std::abs(inRings(progCol) - prog) < 20 && inRings(dataCol) == 0 && inRings(dim) > data,
              "... the programs as they were, everything else dimmed");
        check(hipi::mediamap_tap(700, rowY) == hipi::MediaMapTap::Handled && hipi::mediamap_highlight() == -1 &&
              inRings(dataCol) == data, "a tap on it again: all as before");
        // The BY MACHINE tab
        check(hipi::mediamap_tap(598 + 20 + 72 + 8 + 30, 26 + 20) == hipi::MediaMapTap::Handled && hipi::mediamap_byMachine(),
              "a tap on BY MACHINE: by machine");
        const std::uint32_t c71 = swatch(1);
        check(inRings(c71) > 1000, "... the HP-71 files in their colour");
        hipi::mediamap_tap(700, rowY + 26);
        check(hipi::mediamap_highlight() == 11, "a tap on the HP-71 row: HP-71 highlighted");
        snapshot("mediamap_5_machine.png");
        // SECRET (BAS71 (SP)): blocks 10..49 of the outer ring (308 blocks a ring)
        const double a = -M_PI / 2 + (30.5 / 308) * 2 * M_PI;
        hipi::mediamap_tap(static_cast<int>(cx + 265 * std::cos(a)), static_cast<int>(cy + 265 * std::sin(a)));
        snapshot("mediamap_6_file71.png");
        bool lock = false;                       // the padlock, yellow, after "BAS71 (SP)"
        const int fy = 26 + 20 + 22 + 9 * 26 + 32 + 22 + 22 + 26;
        for (int x = 598 + 16 + 112 + 80; x < 598 + 16 + 112 + 80 + 40 && !lock; ++x)
            for (int y = fy; y < fy + 16 && !lock; ++y) lock = at(x, y) == at(598 + 2, 300);
        check(lock, "the file's box: BAS71 (SP) with a padlock");
        check(hipi::mediamap_tap(598 + 30, 26 + 20) == hipi::MediaMapTap::Handled && !hipi::mediamap_byMachine() &&
              hipi::mediamap_highlight() == -1, "BY TYPE again: nothing highlighted");
    }

    // No medium
    drive.file = "";
    run(300);
    {
        bool any = false;
        for (int x = 300; x < 724 && !any; ++x) for (int y = 270; y < 310 && !any; ++y) any = at(x, y) == 0xFFFFFF;
        check(any, "no medium: a message");
    }
    haveDrive = false;
    run(300);

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
