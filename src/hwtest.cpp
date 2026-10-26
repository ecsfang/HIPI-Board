// hwtest.cpp -- see hwtest.h
#define MODULE "HWTEST"
#include "hwtest.h"
#include "hwtest_io.h"
#include "i2c_ident.h"
#include "plotterview.h"
#include "boardui.h"
#include "pico/time.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hipi {

namespace {

DisplayDriver* d_ = nullptr;

// ── Look (as the loop map) ──────────────────────────────────────────────
constexpr std::uint16_t kYellow = 0xEDC0, kBlack = 0x0000, kWhite = 0xFFFF;
constexpr std::uint16_t kGrey = 0x7BEF, kDark = 0x39C7, kCard = 0x10A2;
constexpr std::uint16_t kGreen = 0x07E0, kRed = 0xF800;
constexpr std::uint16_t kMagenta = 0xF81F;   // "fault": apart from red (missing) and yellow (probably)
constexpr std::uint16_t kFileInk = 0x632C;   // the file column: there, but quieter than the rest

// The page: three cards (yellow frames with a tab, as the boot screen):
// LEDS and PIXELS side by side at the top, I2C across the whole width
// below. Positions for each panel.
struct Card { int x, y, w, h; };
struct Layout {
    Card leds, pix, i2c;
    int headH;                   // the buttons on a card's first row
    // LEDS: first row (Fitted, HP-IL), LED buttons, Chase/Off/Plotter, the status line
    int ledHead, ledRow, ledRow2, ledH, ledW, ledGap;
    // PIXELS: first row (Fitted, Count), colour order, dots, colour buttons, hint
    int pixHead, order, orderH, orderX, orderGap, dots, dotGap, dotR, pixRow, pixH, colW, colGap, hint;
    int fitW, countX, minusX, valueX, plusX;
    // I2C: first row (Scan + two lines of text), list, legend
    int i2cHead, list, rowH, rows, legend;
    // I2C list columns
    int cAddr, cDot, cName, cWhat, cState, cDetail, cFile;
};
constexpr Layout L {
    { 16, 46, 484, 204 }, { 516, 46, 492, 204 }, { 16, 278, 992, 294 },
    36,
    64, 116, 170, 42, 56, 66,
    64, 108, 26, 620, 62, 142, 29, 12, 178, 36, 70, 77, 224,
    132, 680, 728, 780, 814,
    296, 346, 26, 7, 534,
    36, 84, 100, 248, 440, 528, 792 };
constexpr int kTitleH = 26;
constexpr int kCloseW = 64;              // the close button (title bar, right)
constexpr int kMaxPixels = 16;           // shown and run on this page
constexpr int kMaxFound = 32;
constexpr std::uint32_t kChaseMs = 150, kRunMs = 60, kScanMs = 3000;
constexpr std::uint8_t kLevel = 96;      // pixel brightness (of 255): bright enough, kind to the 5 V

// ── State ───────────────────────────────────────────────────────────────
bool active_ = false;                    // the page is open
bool dirty_ = false;                     // something wasn't drawn: draw it all
// Until a test of its own starts, the page shows what's on the LEDs and
// pixels -- whoever drives them (TFLEDS, TFPIXEL, HP-IL activity): a
// program can be tried without them fitted. A test takes that section over.
bool ledsTest_ = false, pixTest_ = false;
bool ledSeen_[5] = {};                   // watching: lit now (hwio::ledLevel())
absolute_time_t nextWatch_ = nil_time;
constexpr std::uint32_t kWatchMs = 40;
bool ledOn_[5] = {};
bool chase_ = false;
int chaseStep_ = 0;
absolute_time_t nextChase_ = nil_time;

struct Rgb { std::uint8_t r, g, b; };
Rgb pix_[kMaxPixels] = {};               // what's sent (and shown)
Rgb drawnPix_[kMaxPixels] = {};
int drawnCount_ = -1;
bool run_ = false;
int runStep_ = 0, runColour_ = 0;
absolute_time_t nextRun_ = nil_time;
enum class Solid : std::uint8_t { None, Red, Green, Blue, White } solid_ = Solid::None;

I2cFound found_[kMaxFound];
std::string chipsInfo_;                  // "27 descriptions  (SD card: i2c/chips.txt)"
int foundN_ = 0;
bool answered_[128] = {};
absolute_time_t nextScan_ = nil_time;
bool gridShown_ = false;

constexpr Rgb kColours[4] = { { kLevel, 0, 0 }, { 0, kLevel, 0 }, { 0, 0, kLevel }, { kLevel, kLevel, kLevel } };

// The colour byte orders, as pixels.h's kPixelOrders (hwio::pixelOrder())
constexpr const char* kOrderNames[6] = { "RGB", "GRB", "BRG", "RBG", "GBR", "BGR" };

// ── Buttons ─────────────────────────────────────────────────────────────
enum class Id : std::uint8_t {
    LedFitted, HpilLeds, PlotterLed, Led1, Led2, Led3, Led4, Led5, Chase, LedOff,
    PixFitted, Minus, Plus, Run, Red, Green, Blue, White, PixOff,
    Order0, Order1, Order2, Order3, Order4, Order5,      // the colour byte order (radio buttons)
    Scan,
    Close,                                               // the X in the title bar: leaves the page
};
struct TestButton { Id id; int x, y, w, h; };

const std::vector<TestButton>& buttons() {
    static std::vector<TestButton> b;
    if (!b.empty()) return b;
    const int lx = L.leds.x + 16, px = L.pix.x + 16;
    b.push_back({ Id::LedFitted, lx, L.ledHead, L.fitW, L.headH });
    b.push_back({ Id::HpilLeds, lx + L.fitW + 12, L.ledHead, 208, L.headH });
    for (int i = 0; i < 5; ++i)
        b.push_back({ static_cast<Id>(static_cast<int>(Id::Led1) + i), lx + i * L.ledGap, L.ledRow, L.ledW, L.ledH });
    b.push_back({ Id::Chase, lx, L.ledRow2, 96, L.ledH });
    b.push_back({ Id::LedOff, lx + 96 + 12, L.ledRow2, 72, L.ledH });
    b.push_back({ Id::PlotterLed, lx + 96 + 12 + 72 + 12, L.ledRow2, L.leds.x + L.leds.w - 16 - (lx + 96 + 12 + 72 + 12), L.ledH });
    b.push_back({ Id::PixFitted, px, L.pixHead, L.fitW, L.headH });
    b.push_back({ Id::Minus, L.minusX, L.pixHead, 40, L.headH });
    b.push_back({ Id::Plus, L.plusX, L.pixHead, 40, L.headH });
    const Id cols[] = { Id::Run, Id::Red, Id::Green, Id::Blue, Id::White, Id::PixOff };
    for (int i = 0; i < 6; ++i) b.push_back({ cols[i], px + i * L.colGap, L.pixRow, L.colW, L.pixH });
    for (int i = 0; i < 6; ++i)
        b.push_back({ static_cast<Id>(static_cast<int>(Id::Order0) + i), L.orderX + i * L.orderGap, L.order,
                      L.orderGap - 6, L.orderH });
    b.push_back({ Id::Scan, L.i2c.x + 16, L.i2cHead, 120, L.headH });
    // The close button: drawn in the title bar, touched a little below it too
    b.push_back({ Id::Close, SCREEN_MAX_X - kCloseW, 0, kCloseW, kTitleH + 12 });
    return b;
}

bool isOrder(Id id) { return id >= Id::Order0 && id <= Id::Order5; }
int orderOf(Id id) { return static_cast<int>(id) - static_cast<int>(Id::Order0); }

const TestButton* find(Id id) {
    for (const TestButton& b : buttons()) if (b.id == id) return &b;
    return nullptr;
}

// ── Drawing helpers ─────────────────────────────────────────────────────

void text(int x, int y, const char* s, std::uint8_t scale, std::uint16_t fg, std::uint16_t bg) {
    d_->txtSize(scale);
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s);
}

// s, padded with spaces (or cut) to n characters
std::string pad(const char* s, std::size_t n) {
    std::string t(s);
    t.resize(n, ' ');
    return t;
}

bool visible() {
    return d_ != nullptr && active_ && plotterview_output() == DisplayOutput::HwTest &&
           !plotterview_isSplashVisible();
}

// A small change may be drawn now (not while the menu draws elsewhere / covers it)
bool canDrawPart() {
    if (!visible() || gridShown_) return false;
    if (d_->canvasAddr() != 0) return false;
    return true;
}

std::uint16_t rgb565(const Rgb& c) {
    // Shown as bright as the panel can (the strip itself runs dimmer)
    const int m = std::max<int>({ c.r, c.g, c.b, 1 });
    const int r = c.r * 255 / m, g = c.g * 255 / m, b = c.b * 255 / m;
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Watching: as bright as the strip is sent it (a fading tail fades), lifted
// a little so dim levels still show
bool watchingPix() { return !pixTest_; }
std::uint16_t rgb565Seen(const Rgb& c) {
    auto lift = [](int v) { return v == 0 ? 0 : 64 + v * 191 / 255; };
    const int r = lift(c.r), g = lift(c.g), b = lift(c.b);
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

std::string label(Id id) {
    switch (id) {
        case Id::LedFitted: return hwio::ledsFitted() ? "Fitted: YES" : "Fitted: NO";
        case Id::PixFitted: return hwio::pixelsFitted() ? "Fitted: YES" : "Fitted: NO";
        case Id::HpilLeds: {
            static const char* const kMode[] = { "HP-IL on LED 4: OFF", "HP-IL on LED 4: ALL", "HP-IL on LED 4: MESSAGES" };
            const int m = hwio::hpilLeds();
            return kMode[m >= 0 && m <= 2 ? m : 2];
        }
        case Id::PlotterLed: return hwio::plotterLed() ? "Plotter error on LED 3: ON" : "Plotter error on LED 3: OFF";
        case Id::Chase:     return "Chase";
        case Id::LedOff:
        case Id::PixOff:    return "Off";
        case Id::Minus:     return "-";
        case Id::Plus:      return "+";
        case Id::Run:       return "Run";
        case Id::Red:       return "Red";
        case Id::Green:     return "Green";
        case Id::Blue:      return "Blue";
        case Id::White:     return "White";
        case Id::Scan:      return "Scan I2C";
        case Id::Order0: case Id::Order1: case Id::Order2: case Id::Order3: case Id::Order4: case Id::Order5:
            return kOrderNames[orderOf(id)];
        default: {
            char b[4];
            std::snprintf(b, sizeof(b), "%d", static_cast<int>(id) - static_cast<int>(Id::Led1) + 1);
            return b;
        }
    }
}

bool isLed(Id id) { return id >= Id::Led1 && id <= Id::Led5; }
bool inLeds(Id id) { return isLed(id) || id == Id::Chase || id == Id::LedOff; }
bool inPixels(Id id) { return id >= Id::Minus && id <= Id::Order5; }

// LED 5 shares GPIO26 with the pixel strip: no LED 5 while pixels are fitted
bool led5Usable() { return !hwio::pixelsFitted(); }
int ledCount() { return led5Usable() ? 5 : 4; }

bool enabled(Id id) {
    if (id == Id::Led5 && !led5Usable()) return false;
    if (inLeds(id)) return hwio::ledsFitted();
    if (inPixels(id)) {
        if (!hwio::pixelsFitted()) return false;
        if (id == Id::Minus) return hwio::pixelCount() > 1;
        if (id == Id::Plus) return hwio::pixelCount() < kMaxPixels;
    }
    return true;
}

bool lit(Id id) {
    if (isLed(id)) {
        const int i = static_cast<int>(id) - static_cast<int>(Id::Led1);
        return ledsTest_ ? ledOn_[i] : ledSeen_[i];
    }
    switch (id) {
        case Id::Chase: return chase_;
        case Id::Run:   return run_;
        case Id::Red:   return solid_ == Solid::Red;
        case Id::Green: return solid_ == Solid::Green;
        case Id::Blue:  return solid_ == Solid::Blue;
        case Id::White: return solid_ == Solid::White;
        case Id::Scan:  return gridShown_;
        case Id::Order0: case Id::Order1: case Id::Order2: case Id::Order3: case Id::Order4: case Id::Order5:
            return hwio::pixelOrder() == orderOf(id);
        default:        return false;
    }
}

void drawButton(const TestButton& b) {
    const bool on = enabled(b.id), hot = on && lit(b.id);
    if (b.id == Id::Close) {
        // Black on the yellow title bar, a white X in it
        const int x = b.x + 6, y = 2, w = b.w - 8, h = kTitleH - 4;
        d_->fillRoundRect(x, y, w, h, 6, kBlack);
        const int cx = x + w / 2, cy = y + h / 2;
        auto i16 = [](int v) { return static_cast<std::int16_t>(v); };
        for (int t = -1; t <= 1; ++t) {
            d_->line(i16(cx - 7 + t), i16(cy - 7), i16(cx + 7 + t), i16(cy + 7), kWhite);
            d_->line(i16(cx - 7 + t), i16(cy + 7), i16(cx + 7 + t), i16(cy - 7), kWhite);
        }
        return;
    }
    if (isOrder(b.id)) {
        // A radio button: a ring, filled when chosen, and the order's name
        const int r = L.orderH / 2 - 4, cx = b.x + r + 2, cy = b.y + b.h / 2;
        d_->fillRect(b.x, b.y, b.w, b.h, kBlack);
        d_->fillCircle(cx, cy, r, on ? (hot ? kYellow : kGrey) : kDark);
        d_->fillCircle(cx, cy, r - 2, kBlack);
        if (hot) d_->fillCircle(cx, cy, r - 4, kYellow);
        text(cx + r + 6, b.y + (b.h - 16) / 2, label(b.id).c_str(), 0, on ? (hot ? kYellow : kWhite) : kDark, kBlack);
        return;
    }
    const std::uint16_t fill = hot ? kYellow : kCard;
    const std::uint16_t edge = on ? (hot ? kYellow : kGrey) : kDark;
    const std::uint16_t ink = on ? (hot ? kBlack : kWhite) : kDark;
    d_->fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
    d_->roundRect(b.x, b.y, b.w, b.h, 8, edge);
    const std::string s = label(b.id);
    text(b.x + (b.w - static_cast<int>(s.size()) * 8) / 2, b.y + (b.h - 16) / 2, s.c_str(), 0, ink, fill);
}

void drawButtons(bool (*which)(Id)) {
    for (const TestButton& b : buttons()) if (which(b.id)) drawButton(b);
}

void drawCount() {
    char b[8];
    std::snprintf(b, sizeof(b), "%2d", hwio::pixelCount());
    text(L.countX, L.pixHead + (L.headH - 16) / 2, "Count", 0, hwio::pixelsFitted() ? kGrey : kDark, kBlack);
    text(L.pix.x + 16, L.order + (L.orderH - 16) / 2, "Order", 0, hwio::pixelsFitted() ? kGrey : kDark, kBlack);
    text(L.valueX, L.pixHead + (L.headH - 16) / 2, b, 0, hwio::pixelsFitted() ? kWhite : kDark, kBlack);
}

// The pixels as sent (dots), dimmed past the count. Pixel 1 on the right,
// as they sit on the board (D12..D16, from right to left)
void drawDot(int i) {
    const int cx = L.pix.x + 16 + L.dotR + (kMaxPixels - 1 - i) * L.dotGap, cy = L.dots + L.dotR;
    const bool used = i < hwio::pixelCount() && hwio::pixelsFitted();
    const Rgb& c = pix_[i];
    const bool on = c.r || c.g || c.b;
    d_->fillCircle(cx, cy, L.dotR, used ? kGrey : kDark);
    d_->fillCircle(cx, cy, L.dotR - 2, on && used ? (watchingPix() ? rgb565Seen(c) : rgb565(c)) : kBlack);
    drawnPix_[i] = c;
}

void drawDots(bool all) {
    const int count = hwio::pixelCount();
    for (int i = 0; i < kMaxPixels; ++i) {
        const Rgb& a = pix_[i];
        const Rgb& b = drawnPix_[i];
        if (all || count != drawnCount_ || a.r != b.r || a.g != b.g || a.b != b.b) drawDot(i);
    }
    drawnCount_ = count;
}

const char* hintText() {
    if (!pixTest_) {
        if (!hwio::pixelsFitted()) return "";
        return hwio::pixelCount() > kMaxPixels ? "Shows what TFPIXEL sends (the first 16)"
                                               : "Shows what TFPIXEL sends";
    }
    switch (solid_) {
        case Solid::Red:   return "Not red? Choose another order above";
        case Solid::Green: return "Not green? Choose another order above";
        case Solid::Blue:  return "Not blue? Choose another order above";
        case Solid::White: return "A tint: a colour is weak or missing";
        default:           return run_ ? "One pixel runs: red, green, blue, white" : "";
    }
}

// LEDS: whose the LEDs are, under Chase / Off
void drawLedStatus() {
    const int x = L.leds.x + 16;
    const std::size_t n = static_cast<std::size_t>((L.leds.w - 32) / 8);
    const char* t = !hwio::ledsFitted() ? "" : ledsTest_ ? "Test -- Off: back to TFLEDS, HP-IL and the plotter"
                                                         : "Shows what TFLEDS, HP-IL and the plotter send";
    text(x, L.ledRow2 + L.ledH + 10, pad(t, n).c_str(), 0, kGrey, kBlack);
}

void drawHint() {
    const std::size_t n = static_cast<std::size_t>((L.pix.w - 32) / 8);
    text(L.pix.x + 16, L.hint, pad(hintText(), n).c_str(), 0, kGrey, kBlack);
}

std::uint16_t stateColour(I2cState s) {
    switch (s) {
        case I2cState::Ok:       return kGreen;
        case I2cState::Fault:    return kMagenta;
        case I2cState::Probable: return kYellow;
        case I2cState::Missing:  return kRed;
        default:                 return kGrey;
    }
}

const char* stateText(I2cState s) {
    switch (s) {
        case I2cState::Ok:       return "OK";
        case I2cState::Fault:    return "fault";
        case I2cState::Probable: return "probably";
        case I2cState::Missing:  return "missing";
        default:                 return "answers";
    }
}

// Two lines beside Scan: what answers on the bus, and what HIPI knows
void drawI2cHeader() {
    int answered = 0;
    for (int a = 0x08; a <= 0x77; ++a) if (answered_[a]) ++answered;
    char b[96];
    std::snprintf(b, sizeof(b), "%d device%s answer%s   (scanned every %lu s)", answered, answered == 1 ? "" : "s",
                  answered == 1 ? "s" : "", static_cast<unsigned long>(kScanMs / 1000));
    const int x = L.i2c.x + 16 + 120 + 20, vx = x + 13 * 8;
    const std::size_t n = static_cast<std::size_t>((L.i2c.x + L.i2c.w - 16 - vx) / 8);
    const int y = L.i2cHead + L.headH / 2;
    text(x, y - 17, "On the bus:", 0, kGrey, kBlack);
    text(vx, y - 17, pad(b, n).c_str(), 0, kWhite, kBlack);
    text(x, y + 1, "Known chips:", 0, kGrey, kBlack);
    text(vx, y + 1, pad(chipsInfo_.c_str(), n).c_str(), 0, kWhite, kBlack);
}

// The list: padded fields, so a row is overwritten without clearing (no flicker)
void drawList() {
    const int right = L.i2c.x + L.i2c.w - 12;
    const int cols = (right - L.cFile) / 8;
    for (int r = 0; r < L.rows; ++r) {
        const int y = L.list + r * L.rowH;
        const int ty = y + (L.rowH - 16) / 2;
        const bool more = r == L.rows - 1 && foundN_ > L.rows;
        if (r >= foundN_ || more) {
            d_->fillCircle(L.cDot, y + L.rowH / 2, 6, kBlack);
            char b[64] = "";
            if (more) std::snprintf(b, sizeof(b), "+ %d more  (Scan I2C shows every address)", foundN_ - (L.rows - 1));
            else if (r == 0) std::snprintf(b, sizeof(b), "nothing answers on the I2C bus");
            text(L.cAddr, ty, pad(b, static_cast<std::size_t>((right - L.cAddr) / 8)).c_str(), 0, kGrey, kBlack);
            continue;
        }
        const I2cFound& f = found_[r];
        char a[8];
        std::snprintf(a, sizeof(a), "0x%02X", f.addr);
        text(L.cAddr, ty, a, 0, f.state == I2cState::Missing ? kDark : kWhite, kBlack);
        text(L.cAddr + 32, ty, pad("", static_cast<std::size_t>((L.cName - L.cAddr - 32) / 8)).c_str(), 0, kWhite, kBlack);
        d_->fillCircle(L.cDot, y + L.rowH / 2, 6, stateColour(f.state));
        text(L.cName, ty, pad(f.name, static_cast<std::size_t>((L.cWhat - L.cName) / 8)).c_str(), 0, kWhite, kBlack);
        text(L.cWhat, ty, pad(f.what, static_cast<std::size_t>((L.cState - L.cWhat) / 8)).c_str(), 0, kGrey, kBlack);
        text(L.cState, ty, pad(stateText(f.state), static_cast<std::size_t>((L.cDetail - L.cState) / 8)).c_str(), 0,
             stateColour(f.state), kBlack);
        text(L.cDetail, ty, pad(f.detail, static_cast<std::size_t>((L.cFile - L.cDetail) / 8)).c_str(), 0, kWhite, kBlack);
        text(L.cFile, ty, pad(f.file, static_cast<std::size_t>(cols)).c_str(), 0, kFileInk, kBlack);
    }
}

// The i2cdetect grid over the page (Scan)
void drawGrid() {
    const int w = 470, h = 64 + 9 * 20, x = (SCREEN_MAX_X - w) / 2, y = (SCREEN_MAX_Y - h) / 2;
    d_->fillRect(x, y, w, h, kBlack);
    d_->roundRect(x, y, w, h, 12, kYellow);
    d_->roundRect(x + 1, y + 1, w - 2, h - 2, 11, kYellow);
    const char* title = " I2C SCAN ";
    d_->fillRect(x + 16, y - 9, static_cast<int>(std::strlen(title)) * 8 + 8, 18, kYellow);
    text(x + 20, y - 8, title, 0, kBlack, kYellow);
    int n = 0;
    for (int a = 0x08; a <= 0x77; ++a) if (answered_[a]) ++n;
    char line[64];
    std::snprintf(line, sizeof(line), "%d device(s) answer:", n);
    text(x + 20, y + 16, line, 0, kWhite, kBlack);
    text(x + 20, y + 40, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f", 0, kGrey, kBlack);
    for (int row = 0; row < 8; ++row) {
        const int ty = y + 60 + row * 20;
        std::snprintf(line, sizeof(line), "%02x:", row * 16);
        text(x + 20, ty, line, 0, kGrey, kBlack);
        for (int col = 0; col < 16; ++col) {
            const int a = row * 16 + col;
            if (a < 0x08 || a > 0x77) continue;
            std::snprintf(line, sizeof(line), answered_[a] ? "%02x" : "--", a);
            text(x + 20 + 40 + col * 24, ty, line, 0, answered_[a] ? kGreen : kDark, kBlack);
        }
    }
    text(x + w - 120, y + h - 22, "tap to close", 0, kDark, kBlack);
}

// A card: a double yellow frame with rounded corners, its name in a tab on
// the top edge (as on the boot screen)
void card(const Card& c, const char* name) {
    d_->roundRect(c.x, c.y, c.w, c.h, 12, kYellow);
    d_->roundRect(c.x + 1, c.y + 1, c.w - 2, c.h - 2, 11, kYellow);
    const int w = static_cast<int>(std::strlen(name)) * 8 + 16;
    d_->fillRect(c.x + 20, c.y - 9, w, 18, kYellow);
    text(c.x + 28, c.y - 8, name, 0, kBlack, kYellow);
}

void renderFull() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, kBlack);
    d_->fillRect(0, 0, SCREEN_MAX_X, kTitleH, kYellow);
    text(8, 5, "HARDWARE TEST", 0, kBlack, kYellow);
    card(L.leds, "LEDS");
    card(L.pix, "PIXELS");
    card(L.i2c, "I2C");
    for (const TestButton& b : buttons()) drawButton(b);
    drawCount();
    drawDots(true);
    drawHint();
    drawLedStatus();
    drawI2cHeader();
    drawList();
    // Legend
    struct { I2cState s; const char* t; } leg[] = {
        { I2cState::Ok, "OK" }, { I2cState::Fault, "fault" }, { I2cState::Probable, "probably (no ID)" },
        { I2cState::Unknown, "unknown" }, { I2cState::Missing, "missing" } };
    int x = L.cAddr;
    for (auto& l : leg) {
        d_->fillCircle(x + 6, L.legend + 8, 6, stateColour(l.s));
        text(x + 18, L.legend, l.t, 0, kGrey, kBlack);
        x += 18 + static_cast<int>(std::strlen(l.t)) * 8 + 24;
    }
    if (gridShown_) drawGrid();
}

// ── Hardware ────────────────────────────────────────────────────────────

void sendPixels() {
    const int n = hwio::pixelCount();
    for (int i = 0; i < kMaxPixels && i < n; ++i) hwio::setPixel(i, pix_[i].r, pix_[i].g, pix_[i].b);
    hwio::showPixels();
}

// A test starts: the page takes the LEDs / pixels over (all off first)
void startLedTest() {
    if (ledsTest_) return;
    hwio::takeLeds();
    ledsTest_ = true;
    for (bool& b : ledOn_) b = false;
}
void startPixTest() {
    if (pixTest_) return;
    hwio::takePixels();
    pixTest_ = true;
    for (Rgb& c : pix_) c = Rgb{ 0, 0, 0 };
}
// ... and ends (Off, Fitted, leaving the page): off, given back, watched again
void endLedTest() {
    chase_ = false;
    for (bool& b : ledOn_) b = false;
    if (ledsTest_) hwio::releaseLeds();
    ledsTest_ = false;
}
void endPixTest() {
    run_ = false;
    solid_ = Solid::None;
    for (Rgb& c : pix_) c = Rgb{ 0, 0, 0 };
    if (pixTest_) hwio::releasePixels();
    pixTest_ = false;
}

// Watching: what's on the LEDs and pixels now (true: something changed)
bool watchLeds() {
    bool changed = false;
    for (int i = 0; i < 5; ++i) {
        const bool on = hwio::ledsFitted() && (i < 4 || led5Usable()) && hwio::ledLevel(i) > 0.5f;
        if (on != ledSeen_[i]) { ledSeen_[i] = on; changed = true; }
    }
    return changed;
}
void watchPixels() {
    const int n = hwio::pixelsFitted() ? std::min(hwio::pixelCount(), kMaxPixels) : 0;
    for (int i = 0; i < kMaxPixels; ++i) {
        Rgb c{ 0, 0, 0 };
        if (i < n) hwio::pixelShown(i, c.r, c.g, c.b);
        pix_[i] = c;
    }
}

std::string listSignature() {
    std::string s;
    for (int i = 0; i < foundN_; ++i) {
        const I2cFound& f = found_[i];
        s += std::to_string(f.addr) + "|" + std::to_string(static_cast<int>(f.state)) + "|" + f.name + "|" + f.detail + ";";
    }
    return s;
}

void scan() {
    foundN_ = hwio::scanI2c(found_, kMaxFound, answered_);
    nextScan_ = make_timeout_time_ms(kScanMs);
}

// Draws a part now, or remembers to draw everything
template <typename F>
void part(F&& fn) {
    if (canDrawPart()) fn();
    else dirty_ = true;
}

}  // namespace

void hwtest_init(DisplayDriver* display) { d_ = display; }

void hwtest_enter() {
    if (active_) return;
    active_ = true;
    gridShown_ = false;
    chipsInfo_ = hwio::loadI2cChips();       // read again: an edited i2c/*.txt counts at once
    for (bool& b : ledOn_) b = false;
    chase_ = run_ = false;
    solid_ = Solid::None;
    ledsTest_ = pixTest_ = false;            // (watching: nothing is taken over)
    watchLeds();
    watchPixels();
    nextWatch_ = make_timeout_time_ms(kWatchMs);
    drawnCount_ = -1;
    scan();
}

void hwtest_leave() {
    if (!active_) return;
    active_ = false;
    gridShown_ = false;
    endLedTest();                            // (gives back only what a test took)
    endPixTest();
}

void hwtest_drawAll() {
    if (d_ == nullptr || !active_) return;
    dirty_ = false;
    // As the loop map: drawn off-screen, then copied to the panel in one go
    const std::uint32_t prevAddr = d_->canvasAddr();
    const std::uint16_t prevStride = d_->canvasStride();
    d_->beginLayerDraw(LT7683::kBteLayer3Addr);
    renderFull();
    if (prevAddr == 0) d_->endOverlayDraw();
    else               d_->beginLayerDraw(prevAddr, prevStride);
    d_->copyLayerToPanel(LT7683::kBteLayer3Addr, 0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
}

void hwtest_showScan() {
    if (!active_) return;
    scan();
    gridShown_ = true;
    if (visible()) hwtest_drawAll();
    else dirty_ = true;
}

void hwtest_poll() {
    if (!active_) return;
    // LED chase: 1 -> 5 (1 -> 4 while pixels are fitted)
    if (chase_ && time_reached(nextChase_)) {
        nextChase_ = make_timeout_time_ms(kChaseMs);
        const int n = ledCount();
        if (chaseStep_ >= n) chaseStep_ = 0;
        for (int i = 0; i < n; ++i) { ledOn_[i] = i == chaseStep_; hwio::setLed(i, ledOn_[i]); }
        chaseStep_ = (chaseStep_ + 1) % n;
        part([] { drawButtons(isLed); });
    }
    // Pixel run: one pixel along the strip, a new colour every round
    if (run_ && time_reached(nextRun_)) {
        nextRun_ = make_timeout_time_ms(kRunMs);
        const int n = std::min(hwio::pixelCount(), kMaxPixels);
        if (runStep_ >= n) { runStep_ = 0; runColour_ = (runColour_ + 1) % 4; }
        for (int i = 0; i < kMaxPixels; ++i) pix_[i] = i == runStep_ ? kColours[runColour_] : Rgb{ 0, 0, 0 };
        ++runStep_;
        sendPixels();
        part([] { drawDots(false); });
    }
    // Watching: TFLEDS / TFPIXEL (or HP-IL activity) on them
    if (time_reached(nextWatch_)) {
        nextWatch_ = make_timeout_time_ms(kWatchMs);
        if (!ledsTest_ && watchLeds()) part([] { drawButtons(isLed); });
        if (!pixTest_) {
            watchPixels();
            if (hwio::pixelCount() != drawnCount_)          // (TFPIXEL's "0N8")
                part([] { drawCount(); drawButton(*find(Id::Minus)); drawButton(*find(Id::Plus)); drawHint(); });
            part([] { drawDots(false); });
        }
    }
    // I2C: look again now and then
    if (time_reached(nextScan_)) {
        const std::string before = listSignature();
        scan();
        if (listSignature() != before) part([] { drawI2cHeader(); drawList(); });
    }
    if (dirty_ && visible() && !gridShown_) {
        if (d_->canvasAddr() == 0) hwtest_drawAll();
    }
}

void hwtest_demoRun() {
    if (!active_) return;
    // As if Chase and Run were touched (both are off when the page opens)
    for (Id id : { Id::Chase, Id::Run })
        if (const TestButton* b = find(id)) hwtest_tap(b->x + b->w / 2, b->y + b->h / 2);
}

bool hwtest_tap(int x, int y) {
    if (!active_) return false;
    if (gridShown_) {                            // the scan grid: any tap closes it
        gridShown_ = false;
        hwtest_drawAll();
        return true;
    }
    const TestButton* hit = nullptr;
    for (const TestButton& b : buttons())
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) { hit = &b; break; }
    if (hit == nullptr) return false;
    const Id id = hit->id;
    if (!enabled(id)) return true;
    switch (id) {
        case Id::LedFitted:
            hwio::setLedsFitted(!hwio::ledsFitted());
            endLedTest();
            watchLeds();
            part([] { drawButton(*find(Id::LedFitted)); drawButtons(inLeds); drawLedStatus(); });
            break;
        case Id::PlotterLed:
            hwio::setPlotterLed(!hwio::plotterLed());
            part([hit] { drawButton(*hit); });
            break;
        case Id::HpilLeds:
            // Off -> All -> Messages -> Off ...
            hwio::setHpilLeds((hwio::hpilLeds() + 1) % 3);
            part([hit] { drawButton(*hit); });
            break;
        case Id::Chase:
            if (chase_) {
                chase_ = false;
                for (int i = 0; i < 5; ++i) { ledOn_[i] = false; hwio::setLed(i, false); }
            } else {
                startLedTest();
                chase_ = true; chaseStep_ = 0; nextChase_ = get_absolute_time();
            }
            part([] { drawButtons(inLeds); drawLedStatus(); });
            break;
        case Id::LedOff:
            endLedTest();                         // (back to showing TFLEDS)
            watchLeds();
            part([] { drawButtons(inLeds); drawLedStatus(); });
            break;
        case Id::PixFitted:
            hwio::setPixelsFitted(!hwio::pixelsFitted());   // (also hands GPIO26 to the strip or LED 5)
            endPixTest();
            watchPixels();
            ledOn_[4] = false;                    // LED 5 starts off either way
            watchLeds();
            part([] {
                drawButton(*find(Id::PixFitted)); drawButtons(inPixels); drawCount(); drawDots(true); drawHint();
                drawButtons(isLed);
            });
            break;
        case Id::Minus:
        case Id::Plus: {
            const int n = hwio::pixelCount() + (id == Id::Plus ? 1 : -1);
            hwio::setPixelCount(std::max(1, std::min(kMaxPixels, n)));
            if (pixTest_) {
                if (solid_ != Solid::None) {      // the new pixel takes the colour too
                    for (int i = 0; i < kMaxPixels; ++i) pix_[i] = i < hwio::pixelCount() ? pix_[0] : Rgb{ 0, 0, 0 };
                }
                sendPixels();
            } else {
                watchPixels();
            }
            part([] { drawButton(*find(Id::Minus)); drawButton(*find(Id::Plus)); drawCount(); drawDots(true); drawHint(); });
            break;
        }
        case Id::Run:
            if (run_) {
                run_ = false;
                for (Rgb& c : pix_) c = Rgb{ 0, 0, 0 };
                sendPixels();
            } else {
                startPixTest();
                solid_ = Solid::None; run_ = true; runStep_ = 0; runColour_ = 0; nextRun_ = get_absolute_time();
            }
            part([] { drawButtons(inPixels); drawDots(false); drawHint(); });
            break;
        case Id::Red: case Id::Green: case Id::Blue: case Id::White: {
            const int c = static_cast<int>(id) - static_cast<int>(Id::Red);
            startPixTest();
            run_ = false;
            solid_ = static_cast<Solid>(c + 1);
            for (int i = 0; i < kMaxPixels; ++i) pix_[i] = i < hwio::pixelCount() ? kColours[c] : Rgb{ 0, 0, 0 };
            sendPixels();
            part([] { drawButtons(inPixels); drawDots(false); drawHint(); });
            break;
        }
        case Id::PixOff:
            endPixTest();                         // (back to showing TFPIXEL)
            watchPixels();
            part([] { drawButtons(inPixels); drawDots(false); drawHint(); });
            break;
        case Id::Scan:
            hwtest_showScan();
            break;
        case Id::Close:
            plotterview_leaveHwTest();                   // (the page is gone: nothing more to draw)
            return true;
        case Id::Order0: case Id::Order1: case Id::Order2: case Id::Order3: case Id::Order4: case Id::Order5:
            hwio::setPixelOrder(orderOf(id));
            if (pixTest_) sendPixels();                  // what's showing, in the new order
            part([] { drawButtons(inPixels); drawHint(); });
            break;
        default: {
            if (isLed(id)) {
                const int i = static_cast<int>(id) - static_cast<int>(Id::Led1);
                startLedTest();
                if (chase_) {                     // (the chase stops: only this one)
                    chase_ = false;
                    for (int k = 0; k < 5; ++k) { ledOn_[k] = false; hwio::setLed(k, false); }
                }
                ledOn_[i] = !ledOn_[i];
                hwio::setLed(i, ledOn_[i]);
                part([] { drawButtons(inLeds); drawLedStatus(); });
            }
            break;
        }
    }
    return true;
}

}  // namespace hipi
