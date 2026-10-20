// media_create_test.cpp -- File picker -> Create media... on the PC: the
// real dialogs (media_create_ui.cpp) and the image writer (lif_create.cpp)
// with the LT7683 driver against the emulated chip, the "SD card" a folder
// (test/pvstubs/ff.h). Taps go to the dialogs, the main loop is run, the
// file made is checked block by block and read back with lif_info.hpp,
// and the panel is saved:
//
//   media_1_form.png       CREATE MEDIA as it opens
//   media_2_keyboard.png   MEDIA NAME, a name being typed
//   media_3_exists.png     a name that's taken
//   media_4_progress.png   a bigger image being written
//
//   make media_create_test && ./media_create_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "media_create_ui.h"
#include "lif_create.h"
#include "lif_info.hpp"
#include "hp82163_font.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
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
std::string outDir;

void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng((outDir + "/" + png).c_str(), panel.data(), 1024, 600);
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

hipi::MediaCreateUi* ui = nullptr;
std::uint32_t nowMs = 1000;

hipi::MediaAction tapCtl(hipi::MediaCreateUi::Ctl c) {
    int x, y;
    ui->center(c, x, y);
    return ui->tap(x, y);
}
void tapKey(char code) {
    int x, y;
    if (ui->keyCenter(code, x, y)) ui->tap(x, y);
}
void type(const std::string& s) { for (char c : s) tapKey(c); }
// The main loop for `ms`
hipi::MediaAction run(int ms) {
    hipi::MediaAction a = hipi::MediaAction::None;
    for (int t = 0; t < ms && a == hipi::MediaAction::None; t += 10) {
        nowMs += 10;
        a = ui->poll(nowMs);
    }
    return a;
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return 1;
    outDir = cwd;
    // The "SD card": lif/ with two cassettes already
    const std::string sd = outDir + "/media_sd";
    std::filesystem::remove_all(sd);
    std::filesystem::create_directories(sd + "/lif");
    std::ofstream(sd + "/lif/CASS01.DAT") << "x";
    std::ofstream(sd + "/lif/GAMES.DAT") << "x";
    if (chdir(sd.c_str()) != 0) return 1;

    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFont();
    d.fillRect(0, 0, 1024, 600, 0x0000);
    hipi::MediaCreateUi media(&d);
    ui = &media;
    media.setDateSource([](hipi::LifDate& dt) { dt = { 2026, 10, 13, 7, 46, 30 }; return true; });
    using Ctl = hipi::MediaCreateUi::Ctl;
    using A = hipi::MediaAction;

    // ── Sizes ──────────────────────────────────────────────────────────
    const hipi::LifMediaType& cass = hipi::kLifMediaTypes[0];
    check(cass.blocks() == 512 && hipi::kLifMediaTypes[1].blocks() == 2464 && hipi::kLifMediaTypes[6].blocks() == 64000,
          "medium sizes as lifinit's (cassette 512, 9114 2464, 16 MB 64000 blocks)");
    check(hipi::lifDirBlocks(64) == 8 && hipi::lifDirBlocks(65) == 9, "directory: 8 entries per block");
    check(hipi::lifMaxDirEntries(cass) == 170 * 8 && hipi::lifDirChoices(cass).back() == 1024,
          "directory at most a third of the medium (lifinit's limit)");

    // ── CREATE MEDIA ───────────────────────────────────────────────────
    media.open();
    media.draw();
    snapshot("media_1_form.png");
    int fx, fy, fw, fh;
    media.frameRect(fx, fy, fw, fh);
    check(fx + fw <= 900, "the dialog stays left of the button strip");
    check(media.name() == "CASS02", "a name is suggested: the next free CASSnn");
    {
        const std::uint32_t c = at(fx + fw - 30, fy + 15);
        check(((c >> 16) & 0xFF) > 200 && ((c >> 8) & 0xFF) > 160 && (c & 0xFF) < 40, "title band in the menus' yellow");
    }
    tapCtl(Ctl::TypeRight);
    check(media.name() == "DISK01", "another type: the suggestion follows it (DISK01)");
    tapCtl(Ctl::TypeLeft);
    check(media.name() == "CASS02", "... and back");
    // (focus on Type after the taps) Down: Directory, OK: its next size
    media.button(hipi::Button::Down);
    media.button(hipi::Button::Ok);
    media.button(hipi::Button::Up);
    media.button(hipi::Button::Up);                          // back on Name

    // ── MEDIA NAME ─────────────────────────────────────────────────────
    media.button(hipi::Button::Ok);
    check(media.onKeyboard(), "OK on Name: the keyboard");
    for (int i = 0; i < 6; ++i) media.button(hipi::Button::X);   // X deletes the last character
    type("GAMES");
    run(50);
    snapshot("media_2_keyboard.png");
    tapKey(hipi::MediaCreateUi::kOk);
    check(!media.onKeyboard() && media.name() == "GAMES", "typed GAMES, OK: back with the new name");
    snapshot("media_3_exists.png");
    check(tapCtl(Ctl::Create) == A::None && !media.creating() && media.message().find("already exists") != std::string::npos,
          "GAMES.DAT exists: Create refused, said why");
    tapCtl(Ctl::Name);
    tapKey('2');
    tapKey(hipi::MediaCreateUi::kOk);
    check(media.name() == "GAMES2", "GAMES2");
    // Too long, and only the allowed characters
    tapCtl(Ctl::Name);
    type("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    tapKey(hipi::MediaCreateUi::kCancel);
    check(media.name() == "GAMES2", "Cancel on the keyboard: the name as it was");
    tapCtl(Ctl::Name);
    type("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    tapKey(hipi::MediaCreateUi::kOk);
    check(media.name().size() == 24, "at most 24 characters");
    tapCtl(Ctl::Name);
    for (int i = 0; i < 30; ++i) tapKey(hipi::MediaCreateUi::kDel);
    tapKey(hipi::MediaCreateUi::kOk);
    check(media.onKeyboard(), "an empty name isn't taken");
    type("GAMES2");
    tapKey(hipi::MediaCreateUi::kOk);
    check(hipi::lifNameValid("A-B_9") && !hipi::lifNameValid("abc") && !hipi::lifNameValid("A B") &&
          !hipi::lifNameValid(""), "names: A-Z 0-9 - _ only");

    // Not enough space
    stubFreeBytes() = 100 * 1024;
    media.open();
    tapCtl(Ctl::Name); tapKey(hipi::MediaCreateUi::kDel); tapKey(hipi::MediaCreateUi::kDel);
    type("XX"); tapKey(hipi::MediaCreateUi::kOk);
    tapCtl(Ctl::Create);
    check(!media.creating() && media.message().find("space") != std::string::npos, "SD card too full: refused");
    stubFreeBytes() = 8ull << 30;

    // ── Creating a cassette ────────────────────────────────────────────
    media.open();
    tapCtl(Ctl::Name);
    for (int i = 0; i < 6; ++i) tapKey(hipi::MediaCreateUi::kDel);
    type("GAMES2");
    tapKey(hipi::MediaCreateUi::kOk);
    tapCtl(Ctl::Create);
    check(media.creating(), "Create: the file is being written");
    const A done = run(2000);
    check(done == A::Created && media.createdFile() == "GAMES2.DAT", "done: GAMES2.DAT, given to the drive");
    const std::vector<std::uint8_t> img = readFile("lif/GAMES2.DAT");
    check(img.size() == 512 * 256, "full size: 512 blocks (128 kB)");
    if (img.size() == 512 * 256) {
        const std::uint8_t* b = img.data();
        check(b[0] == 0x80 && b[1] == 0x00 && std::memcmp(b + 2, "GAMES2", 6) == 0, "LIF id 0x8000, label GAMES2");
        check(b[11] == 2 && b[19] == 8 && b[21] == 1, "directory at block 2, 8 blocks (64 entries), version 1");
        check(b[27] == 2 && b[31] == 1 && b[34] == 1 && b[35] == 0, "2 tracks, 1 head, 256 sectors");
        check(b[36] == 0x26 && b[37] == 0x10 && b[38] == 0x13 && b[39] == 0x07 && b[40] == 0x46 && b[41] == 0x30,
              "date from the clock: 26-10-13 07:46:30 (BCD, all six bytes)");
        bool dirFF = true, rest0 = true, blk1 = true;
        for (int i = 256; i < 512; ++i) blk1 = blk1 && b[i] == 0;
        for (int i = 2 * 256; i < 11 * 256; ++i) dirFF = dirFF && b[i] == 0xFF;
        for (std::size_t i = 11 * 256; i < img.size(); ++i) rest0 = rest0 && b[i] == 0;
        check(blk1 && dirFF && rest0, "block 1 zeros, directory + first data block 0xFF, the rest zeros");
    }
    std::vector<std::string> lines;
    hipi::lifDescribe("lif/GAMES2.DAT", lines);
    std::string all;
    for (const auto& l : lines) all += l + "\n";
    check(all.find("GAMES2") != std::string::npos && all.find("2026-10-13") != std::string::npos,
          "lif_info reads it: label and date");
    check(hipi::lifdetail::typeName(0xE080) == "PGM41" && hipi::lifdetail::typeName(0xE217) == "BAS71 (SP)" &&
          hipi::lifdetail::typeName(0xE5A5) == "E5A5", "lif_info: type names from lif_types.h, unknown ones in hex");
    std::printf("%s", all.c_str());

    // No clock: an empty date
    media.setDateSource([](hipi::LifDate&) { return false; });
    media.open();
    tapCtl(Ctl::Name);
    for (int i = 0; i < 6; ++i) tapKey(hipi::MediaCreateUi::kDel);
    type("NODATE");
    tapKey(hipi::MediaCreateUi::kOk);
    tapCtl(Ctl::Create);
    run(2000);
    const std::vector<std::uint8_t> nd = readFile("lif/NODATE.DAT");
    check(nd.size() == 512 * 256 && nd[36] == 0 && nd[41] == 0, "no clock: the date stays empty");

    // ── A big one, cancelled half-way ──────────────────────────────────
    media.open();
    for (int i = 0; i < 6; ++i) tapCtl(Ctl::TypeRight);   // HDRIVE1 16 MB
    check(media.name() == "DISK01", "16 MB disk: DISK01");
    tapCtl(Ctl::Create);
    run(1500);
    snapshot("media_4_progress.png");
    check(media.creating() && std::filesystem::exists("lif/DISK01.DAT"), "writing a 16 MB disk ...");
    media.button(hipi::Button::X);
    check(!media.creating() && !std::filesystem::exists("lif/DISK01.DAT"), "X: cancelled, the half-written file deleted");
    // ... and to the end
    tapCtl(Ctl::Create);
    run(100000);
    check(std::filesystem::file_size("lif/DISK01.DAT") == 64000ull * 256, "16 MB disk: 64000 blocks");
    check(tapCtl(Ctl::Cancel) == A::Cancel, "Cancel: leaves");

    if (chdir(cwd) != 0) return 1;
    std::filesystem::remove_all(sd);
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
