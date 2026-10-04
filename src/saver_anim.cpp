#define MODULE "SAVER"
#include "saver_anim.h"
#include "glyph_font.h"
#include "analyzer.h"
#include "config.hpp"
#include "pico/time.h"
#include "pico/rand.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern hipi::Config config;

namespace hipi {

extern const GlyphFont kSaverFontRain;     // src/saver_font_rain.cpp
extern const GlyphFont kSaverGoose40, kSaverGoose56, kSaverGoose72,   // src/saver_font_goose*.cpp
                       kSaverGoose92, kSaverGoose112;

namespace {

DisplayDriver* d_ = nullptr;
AnimKind kind_ = AnimKind::Rain;

int rnd(int lo, int hi) { return lo + static_cast<int>(get_rand_32() % static_cast<std::uint32_t>(hi - lo + 1)); }

// RGB565 colour between a and b (t = 0..15)
std::uint16_t mix(std::uint16_t a, std::uint16_t b, int t) {
    const int ra = (a >> 11) & 31, ga = (a >> 5) & 63, ba = a & 31;
    const int rb = (b >> 11) & 31, gb = (b >> 5) & 63, bb = b & 31;
    return static_cast<std::uint16_t>(((ra + (rb - ra) * t / 15) << 11) |
                                      ((ga + (gb - ga) * t / 15) << 5) | (ba + (bb - ba) * t / 15));
}

bool lcd_ = false;                      // style of the running animation (anim_start())
bool lcdStyle() { return lcd_; }

// ── Rain ────────────────────────────────────────────────────────────────
constexpr int kCellW = 29, kCellH = 27;
constexpr int kCols = SCREEN_MAX_X / kCellW;
constexpr int kRows = SCREEN_MAX_Y / kCellH;
constexpr int kOffX = (SCREEN_MAX_X - kCols * kCellW) / 2;
constexpr int kOffY = (SCREEN_MAX_Y - kRows * kCellH) / 2;
constexpr int kColumnsPerStep = 3;          // keeps each anim_step() short

const char* kMnemonics[] = {
    "RFC", "IDY", "SDA", "SST", "SAI", "SDI", "ETO", "ETE", "NRD", "AAU", "AAD 01", "AAD 02",
    "TAD 01", "TAD 02", "LAD 01", "LAD 03", "UNL", "UNT", "IFC", "DCL", "SDC", "LPD", "DDL 04",
    "DDT 02", "DAB 41", "DAB 0D", "DAB 0A", "END 0A", "ISR", "GTL", "REN", "NRE", "TCT", "CMD" };

struct Column {
    int head;                     // row of the bright head (negative: not in view yet)
    int trail;                    // rows of fading tail
    std::uint32_t periodMs;
    absolute_time_t next;
    std::string stream;           // characters still to fall
};
Column cols_[kCols];
char grid_[kCols][kRows];
int rr_ = 0;                      // round robin over the columns

struct RainColors { std::uint16_t bg, head, c1, c2, c3; };
RainColors rainColors() {
    if (lcdStyle()) {
        const std::uint16_t bg = 0xBE35, ink = 0x18C3;
        return { bg, ink, mix(bg, ink, 12), mix(bg, ink, 8), mix(bg, ink, 4) };
    }
    return { 0x0000, 0xEFFD, 0x07E0, 0x0400, 0x0200 };   // white-green head, greens fading out
}

void resetColumn(Column& c, bool start) {
    c.head = start ? rnd(-kRows, kRows - 1) : rnd(-kRows / 2, -1);
    c.trail = rnd(6, 16);
    c.periodMs = static_cast<std::uint32_t>(rnd(60, 200));
    c.next = make_timeout_time_ms(static_cast<std::uint32_t>(rnd(0, 600)));
    c.stream.clear();
}

char nextChar(Column& c) {
    if (c.stream.empty()) {
        char buf[16];
        // Real traffic first (analyzer.h), made-up mnemonics otherwise
        if (!analyzer_nextFrameText(buf, sizeof(buf)))
            std::snprintf(buf, sizeof(buf), "%s", kMnemonics[rnd(0, static_cast<int>(sizeof(kMnemonics) / sizeof(kMnemonics[0])) - 1)]);
        c.stream = std::string(buf) + "  ";
    }
    const char ch = c.stream.front();
    c.stream.erase(0, 1);
    return ch;
}

void drawCell(int col, int row, std::uint16_t fg, std::uint16_t bg) {
    if (row < 0 || row >= kRows) return;
    drawGlyphChar(d_, kOffX + col * kCellW, kOffY + row * kCellH, kSaverFontRain, grid_[col][row], fg, bg);
}
void eraseCell(int col, int row, std::uint16_t bg) {
    if (row < 0 || row >= kRows) return;
    d_->fillRect(kOffX + col * kCellW, kOffY + row * kCellH, kCellW, kCellH, bg);
}

void rainStart() {
    std::memset(grid_, ' ', sizeof(grid_));
    for (Column& c : cols_) resetColumn(c, true);
}

void rainDrawAll() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, rainColors().bg);
}

void rainStep() {
    const RainColors k = rainColors();
    for (int n = 0; n < kColumnsPerStep; ++n) {
        // The next column (round robin) that is due
        int col = -1;
        for (int i = 0; i < kCols; ++i) {
            const int cidx = (rr_ + i) % kCols;
            if (time_reached(cols_[cidx].next)) { col = cidx; break; }
        }
        if (col < 0) return;
        rr_ = (col + 1) % kCols;
        Column& c = cols_[col];
        c.next = make_timeout_time_ms(c.periodMs);
        const int h = ++c.head;
        if (h - c.trail > kRows) {            // fallen through: start again from the top
            resetColumn(c, false);
            continue;
        }
        if (h >= 0 && h < kRows) {
            grid_[col][h] = nextChar(c);
            drawCell(col, h, k.head, k.bg);
        }
        drawCell(col, h - 1, k.c1, k.bg);
        drawCell(col, h - c.trail / 2, k.c2, k.bg);
        drawCell(col, h - c.trail + 2, k.c3, k.bg);
        eraseCell(col, h - c.trail, k.bg);
    }
}

// ── Geese ───────────────────────────────────────────────────────────────
// The HP-41's "running" goose -- a flock of 1..5 of them, in different
// sizes and speeds, flying across the screen from left to right; then a
// pause, and the next flock. Each goose keeps to its own height band, so
// they never overlap (moving one only redraws its own band).
const GlyphFont* const kGooseFonts[] = { &kSaverGoose40, &kSaverGoose56, &kSaverGoose72,
                                         &kSaverGoose92, &kSaverGoose112 };
constexpr int kGooseSizes = 5;
constexpr int kMaxGeese = 8;
constexpr std::uint32_t kGooseFrameMs = 30;     // a goose moves at most this often
constexpr int kGeesePerStep = 2;                // keeps each anim_step() short
const char kGoose = static_cast<char>(0xFE);

struct GooseImage { std::vector<std::uint16_t> pix; int w = 0, h = 0; };
GooseImage images_[kGooseSizes];                // pre-rendered in the current style
std::uint16_t gooseBg_ = 0;

struct Goose {
    bool active = false;
    int size = 0;
    int y = 0;
    float x = 0;                // left edge (may be off-screen)
    float speed = 0;            // px per ms
    int drawnX = 0;             // where it was last drawn
    bool drawn = false;
    absolute_time_t last = nil_time;
};
Goose geese_[kMaxGeese];
absolute_time_t nextFlock_ = nil_time;
int gooseRr_ = 0;

void renderGooseImages() {
    const bool lcd = lcdStyle();
    const std::uint16_t fg = lcd ? 0x18C3 : 0xFFFF;
    gooseBg_ = lcd ? 0xBE35 : 0x0000;
    std::uint16_t palette[16];
    for (int a = 0; a < 16; ++a) palette[a] = mix(gooseBg_, fg, a);
    for (int s = 0; s < kGooseSizes; ++s) {
        const GlyphFont& f = *kGooseFonts[s];
        const GlyphFont::Glyph* g = f.find(kGoose);
        GooseImage& im = images_[s];
        im.w = g ? g->width : 1;
        im.h = f.height;
        im.pix.assign(static_cast<std::size_t>(im.w * im.h), gooseBg_);
        if (g == nullptr) continue;
        for (int r = 0; r < im.h; ++r) {
            const std::uint8_t* src = f.data + g->offset + r * ((im.w + 1) / 2);
            for (int i = 0; i < im.w; ++i) {
                const std::uint8_t b = src[i / 2];
                im.pix[static_cast<std::size_t>(r * im.w + i)] = palette[(i & 1) ? (b & 0x0F) : (b >> 4)];
            }
        }
    }
}

// Draws the goose image with its left edge at x, clipped to the screen
void drawGooseAt(const GooseImage& im, int x, int y) {
    const int vx = x < 0 ? 0 : x;
    const int vr = (x + im.w > SCREEN_MAX_X) ? SCREEN_MAX_X : x + im.w;
    if (vr <= vx) return;
    d_->drawBitmap565Cropped(static_cast<std::int16_t>(vx), static_cast<std::int16_t>(y),
                             static_cast<std::uint16_t>(vr - vx), static_cast<std::uint16_t>(im.h),
                             static_cast<std::uint16_t>(im.w), im.pix.data() + (vx - x));
}

// Clears the columns [x0, x1) of a band, clipped to the screen
void clearBand(int x0, int x1, int y, int h) {
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_MAX_X) x1 = SCREEN_MAX_X;
    if (x1 > x0) d_->fillRect(x0, y, x1 - x0, h, gooseBg_);
}

// Up to `count` new geese in free slots, each in a height band that no
// goose on screen is using (so they never overlap)
void spawnGeese(int count) {
    for (Goose& g : geese_) {
        if (count <= 0) break;
        if (g.active) continue;
        g.size = rnd(0, kGooseSizes - 1);
        const GooseImage& im = images_[g.size];
        int y = 0;
        bool ok = false;
        for (int tries = 0; tries < 30 && !ok; ++tries) {
            y = rnd(10, SCREEN_MAX_Y - im.h - 10);
            ok = true;
            for (const Goose& o : geese_) {
                if (!o.active || &o == &g) continue;
                if (y < o.y + images_[o.size].h + 6 && o.y < y + im.h + 6) { ok = false; break; }
            }
        }
        if (!ok) return;                         // no free band right now
        g.y = y;
        g.x = static_cast<float>(-im.w - rnd(0, 300));            // staggered start
        g.speed = static_cast<float>(rnd(60, 140) + g.size * 25) / 1000.0f;   // bigger ones a bit faster
        g.drawn = false;
        g.last = get_absolute_time();
        g.active = true;
        --count;
    }
}

// A new group (1..5, more often 2..4) every 0.4..2.5 s while there's room
void spawnFlock() {
    static const int kCountWeights[] = { 15, 25, 25, 20, 15 };
    int r = rnd(0, 99), count = 1;
    for (int i = 0; i < 5; ++i) { if (r < kCountWeights[i]) { count = i + 1; break; } r -= kCountWeights[i]; }
    spawnGeese(count);
    nextFlock_ = make_timeout_time_ms(static_cast<std::uint32_t>(rnd(400, 2500)));
}

void geeseStart() {
    renderGooseImages();
    for (Goose& g : geese_) g.active = false;
    nextFlock_ = make_timeout_time_ms(500);
}

void geeseDrawAll() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, gooseBg_);
    for (Goose& g : geese_) {
        if (!g.active || !g.drawn) continue;
        drawGooseAt(images_[g.size], g.drawnX, g.y);
    }
}

void geeseStep() {
    if (time_reached(nextFlock_)) spawnFlock();
    for (int n = 0; n < kGeesePerStep; ++n) {
        // The next goose (round robin) that is due to move
        Goose* g = nullptr;
        for (int i = 0; i < kMaxGeese; ++i) {
            Goose& c = geese_[(gooseRr_ + i) % kMaxGeese];
            if (c.active && absolute_time_diff_us(c.last, get_absolute_time()) >= kGooseFrameMs * 1000) {
                g = &c;
                gooseRr_ = (gooseRr_ + i + 1) % kMaxGeese;
                break;
            }
        }
        if (g == nullptr) return;
        const absolute_time_t now = get_absolute_time();
        g->x += g->speed * static_cast<float>(absolute_time_diff_us(g->last, now)) / 1000.0f;
        g->last = now;
        const GooseImage& im = images_[g->size];
        const int nx = static_cast<int>(g->x);
        if (nx >= SCREEN_MAX_X) {                // flown off the right edge
            if (g->drawn) clearBand(g->drawnX, g->drawnX + im.w, g->y, im.h);
            g->active = false;
            continue;
        }
        if (nx + im.w <= 0) continue;            // not on screen yet
        if (g->drawn && nx == g->drawnX) continue;
        drawGooseAt(im, nx, g->y);
        if (g->drawn && nx > g->drawnX) clearBand(g->drawnX, nx, g->y, im.h);   // the uncovered part
        g->drawnX = nx;
        g->drawn = true;
    }
}

}  // namespace

void anim_start(AnimKind kind, DisplayDriver* display, bool lcd) {
    d_ = display;
    kind_ = kind;
    lcd_ = lcd;
    if (kind_ == AnimKind::Rain) rainStart();
    else                         geeseStart();
}

void anim_drawAll() {
    if (d_ == nullptr) return;
    if (kind_ == AnimKind::Rain) {
        rainDrawAll();
        std::memset(grid_, ' ', sizeof(grid_));
    } else {
        geeseDrawAll();
    }
}

void anim_step() {
    if (d_ == nullptr) return;
    if (kind_ == AnimKind::Rain) rainStep();
    else                         geeseStep();
}

}  // namespace hipi
