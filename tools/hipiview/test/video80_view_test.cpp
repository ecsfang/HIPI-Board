// video80_view_test.cpp -- TFDISPLAY as an HP 92198 / MC00701A, as HIPI
// runs it: the real Video80 screen (src/video80.cpp), the display device
// (src/display.cpp, its model setting) and the LT7683 driver against the
// emulated chip.
// The controller's frames go to the device, idle() hands the bytes over
// as the main loop would, and the panel is saved as PNGs to look at:
//
//   video80_1_text.png     80 x 24: scrolling, ESC % past row 15, inverse
//   video80_2_roman8.png   the Roman-8 set
//   video80_3_forty.png    40 x 20
//   video80_4_monitor.png  monitor mode (control codes shown)
//   video80_5_small.png    the small font (8x16)
//
// The chip's 12x24 font is a stand-in here (HIPI's 8x16 font, stretched).
//
//   make video80_view_test && ./video80_view_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "hp82163_font.hpp"
#include "Screen.hpp"
#include "display.h"
#include "video80.h"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
bool bTrace = false, bExtTrace = false, usb_connected = true;
hipi::Screen* screen = nullptr;      // (display.cpp's, for the HP 82163 model)

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
hipi::Video80* term = nullptr;
CDisplay* dev = nullptr;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng(png, panel.data(), 1024, 600);
    std::printf("     saved %s\n", png);
}

// The chip's 12x24 font, made from HIPI's 8x16 one (a hollow box for
// what that one doesn't have)
void makeChipFonts() {
    std::uint8_t f8[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f8[c], hipi::font[c - 32], 16);
        else for (int r = 0; r < 16; ++r) f8[c][r] = (r == 2 || r == 13) ? 0x7E : (r > 2 && r < 13 ? 0x42 : 0);
    }
    chip.setBuiltinFont(f8);
    static std::uint16_t f24[256][24];
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 24; ++r) {
            std::uint16_t row = 0;
            for (int x = 0; x < 12; ++x)
                if (f8[c][r * 16 / 24] & (0x80 >> (x * 8 / 12))) row = static_cast<std::uint16_t>(row | (0x8000 >> x));
            f24[c][r] = row;
        }
    chip.setBuiltinFont24(f24);
}

// Lit pixels in a text cell (screen row/col of the view)
int litInCell(int row, int col) {
    int n = 0;
    for (int y = 0; y < term->cellH(); ++y)
        for (int x = 0; x < term->cellW(); ++x) {
            const std::uint32_t p = panel[static_cast<std::size_t>(term->originY() + row * term->cellH() + y) * 1024 +
                                          term->originX() + col * term->cellW() + x] & 0xFFFFFF;
            if (p != 0) ++n;
        }
    return n;
}

// The loop: the frame goes round to the device and comes back
IL_CMD_t send(IL_CMD_t f) { return dev->hpil(f); }

// Data to the device, as a controller sends it (LAD, bytes, UNL), then
// the main loop until it's all on the screen
void out(const std::string& s) {
    send(LAD + 1);
    for (unsigned char c : s) send(c);
    send(UNL);
    while (dev->fifoSize() > 0) dev->idle();
}

// ESC % col row (built by hand: the bytes may be 0)
std::string at(int col, int row) {
    return std::string("\x1b%") + static_cast<char>(col) + static_cast<char>(row);
}

std::string memLine(int line) {
    char buf[hipi::Video80::kMaxCols + 1];
    term->lineText(line, buf);
    std::string s(buf);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFonts();

    term = new hipi::Video80(&d);
    term->setColor(0x07E0);
    screen = new hipi::Screen(&d, 0x07E0, 1, 200, 1024);
    screen->suspend();
    dev = new CDisplay("TFDISPLAY");
    dev->setVideo80(term);
    dev->setEnabled(true);

    // ── As an HP 82163 first (the default) ──────────────────────────
    send(AAU);
    send(AAD + 1);
    send(TAD + 1);
    {
        const IL_CMD_t a = send(SAI);
        check(a == 0x3E, "HP 82163: accessory ID $3E");
        send(a);
    }
    send(UNT);
    out("82163 text\r\n");
    check(term->cell(0, 0) == ' ', "HP 82163: the text goes to Screen, not the 92198 screen");

    // ── The setting: HP 92198 ────────────────────────────────────────
    dev->setModel(DisplayModel::Hp92198);
    check(dev->isVideo80(), "model set to HP 92198");
    term->setActive(true);

    // ── HP-IL: address, identification, status ──────────────────────
    send(AAU);
    check(dev->addr() == 31, "AAU: address 31");
    check(send(AAD + 1) == AAD + 2, "AAD: takes address 1");
    send(TAD + 1);
    const IL_CMD_t aid = send(SAI);
    check(aid == 0x32, "accessory ID 50 ($32, display)");
    check(send(aid) == ETO, "SAI ends with ETO");
    send(UNT);
    {
        send(TAD + 1);
        std::string id;
        IL_CMD_t f = send(SDI);
        for (int i = 0; i < 20 && f != ETO; ++i) { id += static_cast<char>(f & 0xFF); f = send(f); }
        send(UNT);
        check(id == "MC00701A\r\n", "device ID \"MC00701A\" CR LF");
    }
    auto readStatus = [](std::uint8_t& s0, std::uint8_t& s1) {
        send(TAD + 1);
        const IL_CMD_t a = send(SST);
        const IL_CMD_t b = send(a);
        const IL_CMD_t e = send(b);
        send(UNT);
        s0 = static_cast<std::uint8_t>(a);
        s1 = static_cast<std::uint8_t>(b);
        return e == ETO;
    };
    std::uint8_t s0 = 0, s1 = 0;
    check(readStatus(s0, s1) && s0 == 0x80 && s1 == 0x00, "SST: $80, $00, ETO (80 columns, ASCII, cursor on)");

    // ── 80 x 24: scrolling through the memory ────────────────────────
    std::string text;
    for (int i = 0; i < 30; ++i) {
        char l[90];
        std::snprintf(l, sizeof l, "Line %02d  The quick brown fox jumps over the lazy dog -- 0123456789", i);
        text += l;
        if (i < 29) text += "\r\n";
    }
    out(text);
    check(term->windowTop() == 6, "30 lines: the window moved down 6 lines");
    check(memLine(0) .rfind("Line 00", 0) == 0, "the first line is still in the memory");
    check(term->cursorRow() == 23, "cursor on the window's last line");

    // ESC % past row 15 (the 82163 wraps there; this one doesn't)
    out(at(10, 20) + "ROW 20");
    check(term->cursorRow() == 20 && term->cursorCol() == 16, "ESC % col 10 row 20 (+6 chars)");
    check(memLine(6 + 20).find("ROW 20") == 10, "written at row 20 col 10");
    out(at(80, 48) + "*");
    check(memLine(6 + 23).find('*') == 79, "ESC % beyond the screen: its edge (79, 23)");

    // Inverse video: 128..255 in the ASCII set
    out(at(0, 2) + "\xC9\xCE\xD6\xC5\xD2\xD3\xC5\xA0\xD6\xC9\xC4\xC5\xCF");
    chip.render(panel.data(), 0.0);
    check(litInCell(2, 7) > term->cellW() * term->cellH() * 3 / 4, "inverse space is a solid block");
    check(litInCell(2, 20) < term->cellW() * term->cellH() / 2, "normal text after it");

    // A long line, inserted into and deleted from
    out(at(0, 5) + "\x1bK" + at(0, 6) + "\x1bK" + at(0, 5));
    std::string longLine;
    for (int i = 0; i < 100; ++i) longLine += static_cast<char>('A' + i % 26);
    out(longLine);
    const int ln = 6 + 5;
    check(term->continued(ln + 1) && memLine(ln + 1) == longLine.substr(80) , "100 characters: the line goes on below");
    out(at(0, 5) + "\x1bN" "123");
    check(memLine(ln).substr(0, 6) == "123ABC" && memLine(ln + 1).substr(0, 4) == "ZABC", "insert: pushed on into the continuation");
    out("\x1b" "R" + at(0, 5) + "\x1bO\x1bO\x1bO");
    check(memLine(ln).substr(0, 3) == "ABC" && memLine(ln + 1).substr(0, 3) == "CDE", "delete: pulled back from the continuation");
    readStatus(s0, s1);
    check(s1 == 0x00, "status after ESC R: replace mode");
    out("\x1bN");
    readStatus(s0, s1);
    check(s1 == 0x10, "status: insert mode (bit 4)");
    out("\x1bR\x1b<");
    readStatus(s0, s1);
    check(s1 == 0x08, "status: cursor off (bit 3)");
    out("\x1b>");
    out(at(0, 23) + "Last line");
    snapshot("video80_1_text.png");
    chip.render(panel.data(), 0.0);
    check(litInCell(20, 10) > 10 && litInCell(20, 15) > 10, "\"ROW 20\" on the panel at row 20, column 10");

    // LF at the very end of the memory: everything moves up
    for (int i = 0; i < 30; ++i) out("\r\nmore");
    check(term->windowTop() == hipi::Video80::kBufLines - 24, "the window at the end of the memory");
    check(memLine(0).rfind("Line 00", 0) != 0 && memLine(1).rfind("Line 13", 0) == 0, "the memory moved up: the first lines are gone");
    {
        chip.render(panel.data(), 0.0);
        check(litInCell(23, 0) > 10 && litInCell(23, 5) == 0, "bottom row: \"more\" (scrolled in the chip)");
        check(litInCell(22, 0) > 10, "the row above: \"more\" too");
    }
    // Looking back, then live again
    term->scrollView(10);
    check(!term->viewIsLive(), "scrolled back");
    term->viewLive();

    // ── Roman-8 ──────────────────────────────────────────────────────
    out("\x1b" "E" "\x1bj");
    readStatus(s0, s1);
    check(s1 == 0x02, "status: Roman-8 (bit 1)");
    out("Roman-8:  ");
    for (int c = 0xA1; c < 0xFF; ++c) out(std::string(1, static_cast<char>(c)));
    out("\r\n\r\nGr\xCF\xDF" "e, Stra\xDE" "e, caf\xC5, \xD4" "ber, se\xD1or");
    snapshot("video80_2_roman8.png");
    check(term->cell(0, 10) == 0xA1, "Roman-8 bytes kept in the memory");
    out("\x1bk");

    // ── 40 x 20 ──────────────────────────────────────────────────────
    out("\x1b]");
    check(term->cols() == 40 && term->rows() == 20, "ESC ]: 40 x 20");
    readStatus(s0, s1);
    check(s1 == 0x01, "status: 40 columns (bit 0)");
    out("HP 92198 80-COLUMN VIDEO\r\n\r\n40 x 20 mode, double width\r\n\r\n\xC9\xCE\xD6\xC5\xD2\xD3\xC5");
    snapshot("video80_3_forty.png");
    chip.render(panel.data(), 0.0);
    check(term->cellW() == 24, "40 columns: cells twice as wide");
    check(litInCell(4, 0) > term->cellW() * term->cellH() / 2, "inverse in 40 columns");

    // ── Monitor mode ─────────────────────────────────────────────────
    out("\x1b[");
    out("\x1bYMonitor: \x01\x02\x03 CR\r LF\n BS\b ESC\x1bH\x1bZ");
    check(!term->monitorMode(), "ESC Z ends monitor mode");
    check(term->cursorRow() == 0 && memLine(0).find("CR") != std::string::npos, "nothing acted on in monitor mode");
    snapshot("video80_4_monitor.png");

    // ── DCL: back to the defaults ────────────────────────────────────
    send(DCL);
    check(term->cols() == 80 && memLine(0).empty() && term->cursorRow() == 0, "DCL: cleared, defaults");

    // ── The settings: the small font, 40 columns after a reset ───────
    out("Small font: 8 x 16, 80 x 24 = 640 x 384 pixels");
    term->setSmallFont(true);
    check(term->cellW() == 8 && term->cellH() == 16, "small font: 8 x 16 cells");
    snapshot("video80_5_small.png");
    chip.render(panel.data(), 0.0);
    check(litInCell(0, 0) > 10, "small font: drawn");
    term->setSmallFont(false);
    term->setDefaultForty(true);
    check(term->cols() == 40, "default 40 columns: set now");
    send(DCL);
    check(term->cols() == 40, "... and after DCL");
    term->setDefaultForty(false);
    check(term->cols() == 80, "back to 80 columns");

    // ── Back to the HP 82163 ─────────────────────────────────────────
    dev->setModel(DisplayModel::Hp82163);
    send(TAD + 1);
    {
        const IL_CMD_t a = send(SAI);
        check(a == 0x3E, "back to HP 82163: accessory ID $3E");
        send(a);
    }
    check(send(SST) == SST, "HP 82163: SST not answered (passes on)");
    send(UNT);
    dev->setModel(DisplayModel::Hp92198);

    // ── Off the panel: only the memory changes, drawn when shown again
    term->setActive(false);
    out("hidden\r\n");
    term->setActive(true);
    chip.render(panel.data(), 0.0);
    check(litInCell(0, 0) > 10, "shown again: what came meanwhile is drawn");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
