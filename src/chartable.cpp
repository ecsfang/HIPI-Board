#include "chartable.h"
#include "glyph_font.h"
#include "page_chrome.h"     // the page's X and menu button
#include "hp41_segments.h"   // the details box: a character drawn large
#include "printer_font.h"    // ... and as the printer prints it
#include "menu_frame.h"
#include "display_mirror.h"  // the drawn table: a layer a PC viewer needs
#include <cstdio>
#include <cstring>
#include <string>

namespace hipi {

extern const GlyphFont kCharTableFont;     // src/chartable_font.cpp

namespace {
DisplayDriver* d_ = nullptr;
constexpr std::uint16_t kYellow = 0xEDC0, kBlack = 0x0000, kWhite = 0xFFFF;
constexpr std::uint16_t kGrey = 0x7BEF, kGrid = 0x2104;
constexpr std::uint16_t kDimMask = 0x39E7;   // the table behind a character's box: ANDed with this
constexpr std::uint16_t kBoxBg = 0x18E3, kButton = 0x2945;

// The table: 8 rows of 16 cells, code = row * 16 + column (above the
// page's menu button: kPageContentBottom)
constexpr int kTop = 30, kCellW = SCREEN_MAX_X / 16, kCellH = (kPageContentBottom - kTop - 2) / 8;

// The details box (a touched character)
int selected_ = -1;                       // its code, -1: none
constexpr int kBoxX = 132, kBoxY = 64, kBoxW = 760, kBoxH = 436, kBoxTitleH = 34;
constexpr int kCharX = kBoxX + 24, kCharY = kBoxY + kBoxTitleH + 22, kCharW = 300, kCharH = 290;
constexpr int kInfoX = kBoxX + 352;
constexpr int kBtnY = kBoxY + kBoxH - 62, kBtnW = 116, kBtnH = 42, kBtnGap = 14;
struct Box { int x, y, w, h; bool in(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
Box button(int i) { return Box{ kInfoX + i * (kBtnW + kBtnGap), kBtnY, kBtnW, kBtnH }; }   // 0 <, 1 CLOSE, 2 >
Box closeX() { return Box{ kBoxX + kBoxW - 66, kBoxY + 4, 58, kBoxTitleH - 6 }; }

void text(int x, int y, const char* s, std::uint16_t fg, std::uint16_t bg) {
    d_->txtSize(0);
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s);
}
std::int16_t i16(int v) { return static_cast<std::int16_t>(v); }

// ASCII's name for a code: 'A', or a control character's name
std::string asciiName(int code) {
    static const char* const kCtrl[32] = { "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS", "HT", "LF",
                                           "VT", "FF", "CR", "SO", "SI", "DLE", "DC1", "DC2", "DC3", "DC4", "NAK",
                                           "SYN", "ETB", "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US" };
    if (code < 32) return std::string(kCtrl[code]) + "  (a control code)";
    if (code == 32) return "space";
    if (code == 127) return "DEL  (a control code)";
    return std::string("'") + static_cast<char>(code) + "'";
}

// The character as the HP-41 display shows it, only its lit segments
// (hp41_segments.h), `scale` pixels per font unit, the cell's lower left
// corner at x,baseline
void drawSegments(int code, int x, int baseline, double scale, std::uint16_t colour) {
    const std::uint32_t lit = kHp41Lit[code & 127];
    for (int k = 0; k < kHp41Shapes; ++k) {
        if (!(lit & (1u << k))) continue;
        for (int t = kHp41ShapeFirst[k]; t < kHp41ShapeFirst[k + 1]; ++t) {
            const Hp41Triangle& tr = kHp41Triangles[t];
            auto px = [&](int fx) { return i16(x + static_cast<int>(fx * scale + 0.5)); };
            auto py = [&](int fy) { return i16(baseline - static_cast<int>(fy * scale + 0.5)); };
            d_->fillTriangle(px(tr.x0), py(tr.y0), px(tr.x1), py(tr.y1), px(tr.x2), py(tr.y2), colour);
        }
    }
}

// What the HP 82162A prints for it (the HP-41 set, printer_font.h): 5 x 7
// dots, `pitch` apart
void drawPrinterChar(int code, int x, int y, int pitch) {
    const std::uint8_t* cols = kPrinterFontHp41[code & 127];
    const int r = pitch / 2 - 1;
    for (int c = 0; c < 5; ++c)
        for (int row = 0; row < 7; ++row)
            d_->fillCircle(i16(x + c * pitch + pitch / 2), i16(y + row * pitch + pitch / 2), static_cast<std::uint16_t>(r),
                           (cols[c] >> row) & 1 ? kWhite : 0x2104);
}

void drawButton(const Box& b, const char* label) {
    d_->fillRoundRect(i16(b.x), i16(b.y), i16(b.w), i16(b.h), 8, kButton);
    d_->roundRect(i16(b.x), i16(b.y), i16(b.w), i16(b.h), 8, kGrey);
    text(b.x + (b.w - static_cast<int>(std::strlen(label)) * 8) / 2, b.y + (b.h - 16) / 2, label, kWhite, kButton);
}

void drawDetails() {
    const int code = selected_;
    // The box: the menus' yellow frame and title band
    d_->fillRoundRect(i16(kBoxX), i16(kBoxY), i16(kBoxW), i16(kBoxH), 14, MenuFrame::Yellow);
    d_->fillRoundRect(i16(kBoxX + 3), i16(kBoxY + kBoxTitleH), i16(kBoxW - 6), i16(kBoxH - kBoxTitleH - 3), 11, kBlack);
    d_->fillRect(i16(kBoxX + 3), i16(kBoxY + kBoxTitleH), i16(kBoxW - 6), 12, kBlack);
    char b[48];
    std::snprintf(b, sizeof(b), "CHARACTER %d", code);
#if HIPI_MENU_TITLE_HP41FONT
    drawGlyphText(d_, kBoxX + 18, kBoxY + (kBoxTitleH - kMenuTitleFont.height) / 2 + 1, kMenuTitleFont, b, kBlack,
                  MenuFrame::Yellow);
    d_->selectBuiltinFont();
#else
    text(kBoxX + 18, kBoxY + 9, b, kBlack, MenuFrame::Yellow);
#endif
    const Box x = closeX();
    d_->fillRoundRect(i16(x.x), i16(x.y), i16(x.w), i16(x.h), 6, kBlack);
    const int cx = x.x + x.w / 2, cy = x.y + x.h / 2;
    for (int t = -1; t <= 1; ++t) {
        d_->line(i16(cx - 7 + t), i16(cy - 7), i16(cx + 7 + t), i16(cy + 7), kWhite);
        d_->line(i16(cx - 7 + t), i16(cy + 7), i16(cx + 7 + t), i16(cy - 7), kWhite);
    }
    // The character, large: only the lit segments
    d_->fillRoundRect(i16(kCharX), i16(kCharY), i16(kCharW), i16(kCharH), 10, kBoxBg);
    const double scale = 0.26;                                  // (font units -> pixels: a cell 234 x 213)
    const int ox = kCharX + (kCharW - static_cast<int>(kHp41CellW * scale)) / 2;   // (the cell centred)
    const int base = kCharY + 24 + static_cast<int>(kHp41FontH * scale);
    drawSegments(code, ox, base, scale, kWhite);
    // What it is
    int y = kCharY + 6;
    auto row = [&](const char* k, const std::string& v) {
        text(kInfoX, y, k, kGrey, kBlack);
        text(kInfoX + 120, y, v.c_str(), kWhite, kBlack);
        y += 36;
    };
    std::snprintf(b, sizeof(b), "%d", code);
    row("Decimal", b);
    std::snprintf(b, sizeof(b), "%02X", code);
    row("Hex", b);
    std::string bin;
    for (int i = 7; i >= 0; --i) { bin += (code >> i) & 1 ? '1' : '0'; if (i == 4) bin += ' '; }
    row("Binary", bin);
    row("In ASCII", asciiName(code));
    text(kInfoX, y, "Printer", kGrey, kBlack);
    drawPrinterChar(code, kInfoX + 120, y - 2, 12);
    text(kInfoX + 120 + 5 * 12 + 18, y + 32, "(HP 82162A)", kGrey, kBlack);
    // < previous, CLOSE, next >
    std::snprintf(b, sizeof(b), "<  %d", (code + 127) % 128);
    drawButton(button(0), b);
    drawButton(button(1), "CLOSE");
    std::snprintf(b, sizeof(b), "%d  >", (code + 1) % 128);
    drawButton(button(2), b);
}

// The table (into the current canvas -- kCharTableAddr, drawAll())
void renderTable() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, kBlack);
    d_->fillRect(0, 0, SCREEN_MAX_X, 26, kYellow);
    text(8, 5, "HP-41 CHARACTER TABLE", kBlack, kYellow);
    text(240, 5, "the HP-41's character codes 0-127 (decimal) as its display shows them", kBlack, kYellow);
    page_drawClose(d_, 26);

    // The HP-41's own codes, characters as its display shows them
    // (src/chartable_font.cpp is indexed by HP-41 code)
    for (int i = 0; i <= 16; ++i)
        d_->fillRect(i * kCellW - (i == 16 ? 1 : 0), kTop, 1, kCellH * 8, kGrid);
    for (int i = 0; i <= 8; ++i)
        d_->fillRect(0, kTop + i * kCellH, SCREEN_MAX_X, 1, kGrid);
    char num[8];
    for (int code = 0; code < 128; ++code) {
        const int x = (code % 16) * kCellW, y = kTop + (code / 16) * kCellH;
        std::snprintf(num, sizeof(num), "%d", code);
        text(x + 4, y + 4, num, kGrey, kBlack);
        const GlyphFont::Glyph* g = kCharTableFont.find(static_cast<char>(code));
        if (g == nullptr) continue;
        // The character in the cell's lower right part (one glyph = one box)
        const int gx = x + kCellW - 6 - g->width;
        const int gy = y + kCellH - 6 - kCharTableFont.height;
        drawGlyphChar(d_, gx, gy, kCharTableFont, static_cast<char>(code), kWhite, kBlack);
    }
    text(8, SCREEN_MAX_Y - 26, "Touch a character for more.  X (top right) or a swipe closes the table.", kGrey, kBlack);
    page_drawMenu(d_);
}

// The screen from the table drawn once (kCharTableAddr), by the display
// chip: as it is -- or, with a character's box, dimmed (ANDed with a grey:
// in layer 3), the touched cell framed and the box drawn over it -- then
// copied to the panel in one go. Drawing the table again (128 characters
// and their numbers) is what took the time.
void compose() {
    const std::uint32_t prevAddr = d_->canvasAddr();
    const std::uint16_t prevStride = d_->canvasStride();
    if (selected_ < 0) {
        d_->copyLayerRegion(LT7683::kCharTableAddr, SCREEN_MAX_X, 0, 0, 0, SCREEN_MAX_X, 0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
    } else {
        // The grey to AND with: in layer 2 (a scratch layer)
        d_->beginLayerDraw(LT7683::kBteLayer2Addr);
        d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
        d_->fillRect(0, 0, SCREEN_MAX_X, kPageContentBottom, kDimMask);
        d_->copyLayerRegion(LT7683::kCharTableAddr, SCREEN_MAX_X, 0, 0, LT7683::kBteLayer3Addr, SCREEN_MAX_X, 0, 0,
                            SCREEN_MAX_X, SCREEN_MAX_Y);
        // (the title bar and the menu button's row stay bright: the page's buttons)
        d_->andLayerRegion(LT7683::kBteLayer2Addr, SCREEN_MAX_X, 0, 28, LT7683::kBteLayer3Addr, SCREEN_MAX_X, 0, 28,
                           SCREEN_MAX_X, static_cast<std::uint16_t>(kPageContentBottom - 28));
        d_->beginLayerDraw(LT7683::kBteLayer3Addr);
        d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
        d_->selectBuiltinFont();
        const int x = (selected_ % 16) * kCellW, y = kTop + (selected_ / 16) * kCellH;
        for (int k = 0; k < 3; ++k) d_->rect(i16(x + k), i16(y + k), i16(kCellW + 1 - 2 * k), i16(kCellH + 1 - 2 * k), kYellow);
        drawDetails();
        d_->copyLayerToPanel(LT7683::kBteLayer3Addr, 0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
    }
    if (prevAddr == 0) d_->endOverlayDraw();
    else               d_->beginLayerDraw(prevAddr, prevStride);
}
}  // namespace

void chartable_init(DisplayDriver* display) { d_ = display; }

void chartable_drawAll() {
    if (d_ == nullptr) return;
    // The table in its own layer (the slow part: once), then the screen from it
    const std::uint32_t prevAddr = d_->canvasAddr();
    const std::uint16_t prevStride = d_->canvasStride();
    d_->beginLayerDraw(LT7683::kCharTableAddr);
    renderTable();
    if (prevAddr == 0) d_->endOverlayDraw();
    else               d_->beginLayerDraw(prevAddr, prevStride);
    // A viewer that connects later needs it (the boxes are made from it):
    // always the same table -- cached by the viewer, per build of this file
    static bool told = false;
    if (!told) {
        std::uint32_t id = 2166136261u;
        for (const char* p = "CHARTABLE " __DATE__ " " __TIME__; *p; ++p) id = (id ^ static_cast<std::uint8_t>(*p)) * 16777619u;
        displayMirror_addLayer(LT7683::kCharTableAddr, SCREEN_MAX_X * 2UL * SCREEN_MAX_Y, 0, 1, id | 1u);
        told = true;
    }
    compose();
}

bool chartable_tap(int x, int y) {
    if (selected_ >= 0) {
        // The box: its buttons; a touch beside it closes it
        if (button(0).in(x, y))      chartable_step(-1);
        else if (button(2).in(x, y)) chartable_step(1);
        else if (button(1).in(x, y) || closeX().in(x, y) || !Box{ kBoxX, kBoxY, kBoxW, kBoxH }.in(x, y))
            chartable_closeDetails();
        return true;
    }
    if (y < kTop || y >= kTop + 8 * kCellH || x < 0 || x >= 16 * kCellW) return false;
    selected_ = ((y - kTop) / kCellH) * 16 + x / kCellW;
    compose();
    return true;
}

bool chartable_detailsOpen() { return selected_ >= 0; }
int chartable_selected() { return selected_; }

void chartable_step(int by) {
    if (selected_ < 0) return;
    selected_ = ((selected_ + by) % 128 + 128) % 128;
    compose();
}

void chartable_closeDetails() {
    if (selected_ < 0) return;
    selected_ = -1;
    compose();
}

void chartable_reset() { selected_ = -1; }

}  // namespace hipi
