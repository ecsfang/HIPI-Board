// demo.cpp -- see demo.h
#define MODULE "DEMO"
#include "demo.h"
#include "demo_content.h"
#include "plotterview.h"
#include "Screen.hpp"
#include "video80.h"
#include "display.h"
#include "plotter.h"
#include "printer.h"
#include "drive.h"
#include "hwtest_io.h"
#include "hwtest.h"
#include "analyzer.h"
#include "loopmap.h"
#include "clock.h"
#include "boardui.h"
#include "backlight.h"
#include "config.hpp"
#include "lif_info.hpp"
#include "mediamap.h"
#include "lif_create.h"
#include "sd_paths.h"
#include "usb_serial.h"
#include "display_mirror.h"
#include "ff.h"
#include "pico/time.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern hipi::Screen* screen;
extern hipi::Video80* video80Term;
extern std::vector<CDevice*> devices;
extern hipi::Config config;
unsigned short hipi_simFrame(unsigned short frame, unsigned short (*pcDevices)(unsigned short));   // hipi.cpp (hipi.h)
extern const std::uint16_t kLogoImageWidth, kLogoImageHeight;
extern const std::uint16_t kLogoImage[];

namespace hipi {


namespace {

DisplayDriver* d_ = nullptr;
DemoHooks hooks_;
const char* version_ = "";

// A step of the demo: called each poll until it says it's done
using Step = std::function<bool(bool first)>;

bool running_ = false;
volatile bool stopRequested_ = false;
std::vector<Step> script_;
std::size_t at_ = 0;
bool first_ = true;
int round_ = 0;

// Where it started (to go back to)
DisplayOutput startOutput_ = DisplayOutput::Display;
CDevice* startDevice_ = nullptr;
// What the demo touched, to put back
std::vector<IL_CMD_t> savedAddr_;
bool lights_ = false;
bool tapeShown_ = false;
bool pickerShown_ = false;
bool createShown_ = false;                  // CREATE MEDIA up (the same overlay as captions)

// The caption: a yellow band at the bottom, in the menu layer (PIP)
bool captionShown_ = false;
std::uint32_t captionUntil_ = 0;          // (demo time, nowMs())
absolute_time_t nextActivity_ = nil_time;

// Waiting for hipiview: with a viewer connected, the demo stands still
// while the mirror sends the whole screen (a session starting or
// restarting) and, before each step, until the viewer has caught up --
// so a recording gets every step whole. Its clock stands still too, so
// every pause and caption keeps its length. No viewer: no waiting.
constexpr std::uint32_t kCaughtUpBytes = 2048;   // backlog small enough to go on
constexpr std::uint32_t kMaxSyncWaitMs = 20000;  // waiting for the whole screen at most this long
constexpr std::uint32_t kMaxCatchUpMs = 3000;    // ... for a backlog (a view that never stops drawing)
std::uint32_t pausedMs_ = 0;                     // waited so far, in all
bool waiting_ = false;
bool gaveUp_ = false;                            // waited the longest: go on until it's caught up
std::uint32_t waitFrom_ = 0;

std::uint32_t realMs() { return to_ms_since_boot(get_absolute_time()); }
// The demo's own clock: real time without the waits
std::uint32_t nowMs() { return (waiting_ ? waitFrom_ : realMs()) - pausedMs_; }
bool captionDue() { return static_cast<std::int32_t>(nowMs() - captionUntil_) >= 0; }

// ── Captions, flash, end card ───────────────────────────────────────────

void hideOverlay() {
    d_->hidePipOverlay();
    captionShown_ = false;
}

void showCaption(const std::string& text, std::uint32_t ms) {
    if (pickerShown_ || createShown_) return;    // (the same overlay)
    const int textW = static_cast<int>(text.size()) * 16;
    const int w = std::min((textW + 48 + 3) & ~3, (SCREEN_MAX_X - 32) & ~3);
    const int h = 32 + 20;
    const int x = ((SCREEN_MAX_X - w) / 2) & ~3;
    const int y = SCREEN_MAX_Y - h - 14;
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->beginOverlayDraw();
    d_->fillRect(x, y, w, h, 0xEDC0);
    d_->rect(x, y, w, h, 0xFFFF);
    d_->selectBuiltinFont();
    d_->txtSize(1);
    d_->txtColor(0x0000, 0xEDC0);
    d_->txtSetCursor(static_cast<std::uint16_t>(x + (w - textW) / 2), static_cast<std::uint16_t>(y + 10));
    d_->txtWrite(text.c_str());
    d_->endOverlayDraw();
    screen->reassertRenderState();
    d_->showPipOverlay(x, y, w, h);
    captionShown_ = true;
    captionUntil_ = nowMs() + ms;
}

// A white flash over the panel (the screendump, as a camera would)
void showFlash() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->beginOverlayDraw();
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0xFFFF);
    d_->endOverlayDraw();
    screen->reassertRenderState();
    d_->showPipOverlay(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
    captionShown_ = true;
    captionUntil_ = nowMs() + 120;
}

// The start-up screen's box: logo, name, version
void showEndCard() {
    d_->beginOverlayDraw();
    showSplashScreen(d_, version_, 0);
    d_->endOverlayDraw();
    screen->reassertRenderState();
    constexpr int w = 400, h = 170;
    d_->showPipOverlay(((SCREEN_MAX_X - w) / 2) & ~3, (SCREEN_MAX_Y - h) / 2, w, h);
    captionShown_ = true;
    captionUntil_ = nowMs() + 7000;
}

// ── Steps ───────────────────────────────────────────────────────────────

Step call(std::function<void()> fn) {
    return [fn](bool) { fn(); return true; };
}

Step wait(std::uint32_t ms) {
    return [ms, until = std::uint32_t(0)](bool first) mutable {
        if (first) until = nowMs() + ms;
        return static_cast<std::int32_t>(nowMs() - until) >= 0;
    };
}

Step caption(const char* text, std::uint32_t ms = 3000) {
    return call([text, ms] { showCaption(text, ms); });
}

Step view(DisplayOutput o, CDevice* dev) {
    return call([o, dev] { plotterview_showView(o, dev); });
}

// Text to the display (82163: Screen) or the 92198 screen, ms per character
enum class Sink : std::uint8_t { Screen, Video80 };
Step type(Sink sink, std::string text, std::uint32_t ms) {
    return [sink, text, ms, i = std::size_t(0), next = std::uint32_t(0)](bool first) mutable {
        if (first) { i = 0; next = nowMs(); }
        while (i < text.size() && static_cast<std::int32_t>(nowMs() - next) >= 0) {
            const auto c = static_cast<std::uint8_t>(text[i++]);
            if (sink == Sink::Screen) screen->pr_char(c);
            else video80Term->put(c);
            next += ms;
            if (ms > 0 && i % 64 == 0) break;            // (a long burst: let the loop run)
        }
        return i >= text.size();
    };
}

// HP-GL to the plotter, n characters every 15 ms
Step hpgl(std::function<std::string()> make, std::size_t n) {
    return [make, n, g = std::string(), i = std::size_t(0), next = std::uint32_t(0)](bool first) mutable {
        if (first) { g = make(); i = 0; next = nowMs(); }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        next = nowMs() + 15;
        for (std::size_t k = 0; k < n && i < g.size(); ++k) plotter->put(g[i++]);
        if (i >= g.size()) { g.clear(); g.shrink_to_fit(); return true; }
        return false;
    };
}

// Things done at set times (ms after the step starts): the demo's
// "touches". build() fills the list when the step starts.
struct Act { std::uint32_t at; std::function<void()> fn; };
Step acts(std::function<void(std::vector<Act>&)> build) {
    return [build, list = std::vector<Act>(), i = std::size_t(0), t0 = std::uint32_t(0)](bool first) mutable {
        if (first) { list.clear(); build(list); i = 0; t0 = nowMs(); }
        while (i < list.size() && nowMs() - t0 >= list[i].at) list[i++].fn();
        if (i < list.size()) return false;
        list.clear();
        return true;
    };
}

// Digitizing, as a person would: the plotter waits for a point (DP), a
// touch a little off SIN(X)/X's top, the arrows nudge the marker onto it
// (left held, up tapped), ENTER. The "touches" go to the digitize panel
// directly -- a real one stops the demo.
Step digitizePoint() {
    return acts([](std::vector<Act>& a) {
        auto press = [](int id) {
            int x = 0, y = 0;
            if (plotterview_digitizeControl(id, x, y)) plotterview_digitizeTap(x, y);
        };
        std::uint32_t t = 0;
        a.push_back({ t, [] {
            int x = 0, y = 0;
            plotterview_plotToScreen(demo::kFunctionPeakX, demo::kFunctionPeakY, x, y);
            plotterview_digitizeTap(x + 10, y + 6);
            plotterview_digitizeRelease();
        } });
        // Arrows tapped, never held: a held arrow repeats in real time, and
        // the demo's time stands still while hipiview catches up -- the
        // marker would run on (and the mirror fall further behind) until
        // the release came
        t += 1500;
        for (int k = 0; k < 10; ++k) {                             // left
            t += 220;
            a.push_back({ t, [press] { press(2); } });
            a.push_back({ t + 100, [] { plotterview_digitizeRelease(); } });
        }
        t += 300;
        for (int k = 0; k < 6; ++k) {                              // up, tapped
            t += 450;
            a.push_back({ t, [press] { press(0); } });
            a.push_back({ t + 120, [] { plotterview_digitizeRelease(); } });
        }
        t += 1500;
        a.push_back({ t, [press] { press(4); } });                // ENTER
        a.push_back({ t + 120, [] { plotterview_digitizeRelease(); } });
    });
}

// Printer lines, one every ms
Step print(std::uint32_t ms) {
    return [ms, lines = std::vector<std::string>(), i = std::size_t(0), next = std::uint32_t(0)](bool first) mutable {
        if (first) { lines = demo::printLines(); i = 0; next = nowMs(); }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        next = nowMs() + ms;
        if (i < lines.size()) {
            for (char c : lines[i]) printer->put(static_cast<std::uint8_t>(c));
            ++i;
            return false;
        }
        return true;
    };
}

// ── The cassette ────────────────────────────────────────────────────────

std::vector<std::string> cassettes() {
    std::vector<std::string> v;
    DIR dir;
    if (f_opendir(&dir, HIPI_DIR_LIF) == FR_OK) {
        FILINFO info;
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
            if (info.fattrib & AM_DIR) continue;
            const char* dot = std::strrchr(info.fname, '.');
            if (dot != nullptr && (std::strcmp(dot, ".dat") == 0 || std::strcmp(dot, ".DAT") == 0)) v.push_back(info.fname);
        }
        f_closedir(&dir);
    }
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::string> files_;     // the picker's list ("" = No media first)
int pickFrom_ = 0, pickTo_ = 0;

// Opens the lid, shows the list, moves to another cassette, closes, turns the reels
Step tapePick() {
    return [step = 0, sel = 0, next = std::uint32_t(0)](bool first) mutable {
        CDrive* drv = plotterview_drive();
        if (first) {
            step = 0;
            next = nowMs();
            const std::vector<std::string> c = cassettes();
            files_.assign(1, std::string());
            files_.insert(files_.end(), c.begin(), c.end());
            pickFrom_ = 0;
            const std::string current = drv != nullptr ? drv->mediaFile() : std::string();
            for (std::size_t i = 0; i < files_.size(); ++i) if (files_[i] == current) pickFrom_ = static_cast<int>(i);
            // Another cassette than the one in it (a few rows on, if there are)
            const int n = static_cast<int>(files_.size());
            pickTo_ = n > 1 ? 1 + (pickFrom_ + 2) % (n - 1) : 0;
            if (pickTo_ == pickFrom_ && n > 2) pickTo_ = 1 + pickFrom_ % (n - 1);
            sel = pickFrom_;
        }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        switch (step) {
            case 0:                                   // the lid opens, the list
                tapeShown_ = true;
                plotterview_setTapeEjected(true);
                if (captionShown_) hideOverlay();
                pickerShown_ = true;
                if (hooks_.showPicker) hooks_.showPicker(files_, sel);
                next = nowMs() + 1200;
                ++step;
                return false;
            case 1:                                   // down the list, a row at a time
                if (sel != pickTo_) {
                    sel += sel < pickTo_ ? 1 : -1;
                    if (hooks_.movePicker) hooks_.movePicker(sel);
                    next = nowMs() + 450;
                    return false;
                }
                next = nowMs() + 900;
                ++step;
                return false;
            case 2:                                   // chosen: the lid closes with it
                if (hooks_.hidePicker) hooks_.hidePicker();
                pickerShown_ = false;
                plotterview_setTapeFile(files_[static_cast<std::size_t>(pickTo_)]);
                plotterview_setTapeEjected(false);
                plotterview_setDemoTape(true);        // reading it: BUSY, the reels
                next = nowMs() + 3000;
                ++step;
                return false;
            default:
                plotterview_setDemoTape(false);
                return true;
        }
    };
}

// DIR of the picked cassette on the display
Step dir() {
    return [](bool) {
        const std::string file = pickTo_ < static_cast<int>(files_.size()) ? files_[static_cast<std::size_t>(pickTo_)] : "";
        std::vector<std::string> lines;
        if (!file.empty()) lifDescribe(CDrive::lifPath(file).c_str(), lines);
        const std::string text = demo::textDir(file.empty() ? std::string("(NO MEDIA)") : file, lines);
        for (char c : text) screen->pr_char(static_cast<std::uint8_t>(c));
        return true;
    };
}

// ── The media map of the picked cassette ────────────────────────────────
// It isn't really in the drive: the map gets its own source meanwhile --
// the cassette's file, and a head that reads the directory and then runs
// along the whole tape

bool demoMap_ = false;

Step mediaMap(std::uint32_t ms) {
    return [ms, until = std::uint32_t(0), captioned = false](bool first) mutable {
        if (first) {
            captioned = false;
            const std::string file = pickTo_ > 0 && pickTo_ < static_cast<int>(files_.size())
                                         ? files_[static_cast<std::size_t>(pickTo_)] : std::string();
            if (file.empty()) return true;                // (no medium picked: nothing to map)
            const std::string path = CDrive::lifPath(file);
            std::uint32_t blocks = 0;
            FIL f;
            if (f_open(&f, path.c_str(), FA_READ) == FR_OK) {
                blocks = static_cast<std::uint32_t>(f_size(&f) / 256);
                f_close(&f);
            }
            const std::uint32_t t0 = nowMs();
            mediamap_setDemoSource(
                [file, blocks, t0, ms](MediaMapLive& l) {
                    const std::uint32_t t = nowMs() - t0;
                    l.file = file;
                    l.op = MediaOp::Read;
                    l.writes = 0;
                    l.busy = false;
                    if (t < 1500 || blocks < 4) l.head = 2 + t / 300;        // the directory first
                    else l.head = std::min(blocks - 1, 2 + static_cast<std::uint32_t>(
                                               static_cast<std::uint64_t>(t - 1500) * blocks / std::max<std::uint32_t>(ms - 2000, 1)));
                    return true;
                },
                [path](std::uint32_t blk, std::uint8_t* b) {
                    FIL f;
                    if (f_open(&f, path.c_str(), FA_READ) != FR_OK) return false;
                    UINT br = 0;
                    const bool ok = f_lseek(&f, static_cast<FSIZE_t>(blk) * 256) == FR_OK &&
                                    f_read(&f, b, 256, &br) == FR_OK && br == 256;
                    f_close(&f);
                    return ok;
                });
            demoMap_ = true;
            plotterview_showView(DisplayOutput::MediaMap, nullptr);   // (no name box: the caption)
            until = nowMs() + ms;
            return false;
        }
        if (!captioned) {                                 // (once the view is up)
            showCaption("MEDIA MAP - WHAT'S WHERE ON THE MEDIUM", 3500);
            captioned = true;
        }
        if (static_cast<std::int32_t>(nowMs() - until) < 0) return false;
        mediamap_clearDemoSource();
        demoMap_ = false;
        return true;
    };
}

// ── File picker -> Create media...: filled in, never created ────────────
// Name (the keyboard), type and directory size stepped; then it goes --
// nothing is written. (Ctl numbers: MediaCreateUi::Ctl.)

Step createMedia() {
    return acts([](std::vector<Act>& a) {
        enum { Name = 0, TypeLeft, TypeRight, DirLeft, DirRight };
        const char kDel = 8, kOk = 13;                       // MediaCreateUi's keys
        auto ctl = [](int c) { return [c] { if (hooks_.createMediaControl) hooks_.createMediaControl(c); }; };
        auto key = [](char k) { return [k] { if (hooks_.createMediaKey) hooks_.createMediaKey(k); }; };
        // The caption first: it uses the same overlay as the dialog
        a.push_back({ 0, [] { showCaption("CREATE MEDIA - A NEW, EMPTY CASSETTE OR DISK", 3000); } });
        a.push_back({ 3000, [] {
            if (captionShown_) hideOverlay();
            if (hooks_.showCreateMedia) { hooks_.showCreateMedia(); createShown_ = true; }
        } });
        std::uint32_t t = 5000;
        a.push_back({ t, ctl(Name) });                           // the keyboard
        t += 1200;
        for (int i = 0; i < 6; ++i) { a.push_back({ t, key(kDel) }); t += 180; }
        for (char c : std::string("GAMES2")) { t += 350; a.push_back({ t, key(c) }); }
        t += 900;
        a.push_back({ t, key(kOk) });
        t += 1500;
        a.push_back({ t, ctl(TypeRight) });                      // an HP 9114B disk
        t += 1600;
        a.push_back({ t, ctl(TypeRight) });                      // a 640 kB disk
        t += 1600;
        a.push_back({ t, ctl(DirRight) });                       // a bigger directory
        t += 1400;
        t += 2500;
        a.push_back({ t, [] { if (hooks_.hideCreateMedia) hooks_.hideCreateMedia(); createShown_ = false; } });
        a.push_back({ t + 100, [] { showCaption("CREATE - NOT IN THE DEMO", 2500); } });
        a.push_back({ t + 2700, [] {} });
    });
}

// ── The media map of a made-up disk with HP-41, HP-71 and HP-75 files ──
// Built block by block when read (only the header and the directory are):
// BY TYPE, a row touched (the rest dimmed) and again, BY MACHINE, the
// HP-71 row, then a secure + private HP-71 file touched.

struct DemoFile { const char* name; std::uint16_t type; std::uint32_t len; };
constexpr DemoFile kDemoDisk[] = {
    { "SECRET", 0xE217, 40 }, { "GAME41", 0xE080, 60 }, { "DATA41", 0xE0D0, 30 }, { "KEYS", 0xE050, 3 },
    { "MATH71", 0xE214, 80 }, { "HPILLX", 0xE208, 50 }, { "NOTES", 0x0001, 20 }, { "CALC75", 0xE088, 45 },
    { "SHEET", 0xE08A, 25 }, { "OLDPRG", 0x0000, 35 }, { "PLOT71", 0xE222, 15 }, { "MLDL", 0xE070, 64 },
    { "TXT75", 0xE052, 10 }, { "FORTH", 0xE218, 70 }, { "WALL", 0xE040, 32 }, { "APPTS", 0xE053, 12 } };
constexpr int kDemoDiskEntries = 64;

bool demoDiskBlock(std::uint32_t blk, std::uint8_t* b) {
    const LifMediaType& t = kLifMediaTypes[1];                 // an HP 9114B disk
    std::memset(b, 0, 256);
    const std::uint32_t dirBlocks = static_cast<std::uint32_t>(lifDirBlocks(kDemoDiskEntries));
    if (blk == 0) { lifVolumeHeader(b, t, kDemoDiskEntries, "DEMO", nullptr); return true; }
    if (blk < 2 || blk >= 2 + dirBlocks) return true;
    std::memset(b, 0xFF, 256);
    std::uint32_t at = 2 + dirBlocks;
    const std::size_t n = sizeof(kDemoDisk) / sizeof(kDemoDisk[0]);
    for (std::size_t i = 0; i < n; ++i) {
        const DemoFile& f = kDemoDisk[i];
        if (i / 8 == blk - 2) {
            std::uint8_t* e = b + (i % 8) * 32;
            std::memset(e, 0, 32);
            std::memset(e, ' ', 10);
            std::memcpy(e, f.name, std::min<std::size_t>(10, std::strlen(f.name)));
            e[10] = static_cast<std::uint8_t>(f.type >> 8);
            e[11] = static_cast<std::uint8_t>(f.type);
            for (int k = 0; k < 4; ++k) {
                e[12 + k] = static_cast<std::uint8_t>(at >> (24 - 8 * k));
                e[16 + k] = static_cast<std::uint8_t>(f.len >> (24 - 8 * k));
            }
        }
        at += f.len;
    }
    return true;
}
std::uint32_t demoDiskFirstFile() { return 2 + static_cast<std::uint32_t>(lifDirBlocks(kDemoDiskEntries)); }

Step machineMap() {
    return acts([](std::vector<Act>& a) {
        mediamap_setDemoSource(
            [](MediaMapLive& l) { l.file = "DEMODISK.DAT"; l.op = MediaOp::None; l.writes = 0; l.busy = false; return true; },
            demoDiskBlock);
        demoMap_ = true;
        plotterview_showView(DisplayOutput::MediaMap, nullptr);
        auto row = [](int r) { int x = 0, y = 0; if (mediamap_rowPoint(r, x, y)) mediamap_tap(x, y); };
        auto tab = [](bool m) { int x = 0, y = 0; mediamap_tabPoint(m, x, y); mediamap_tap(x, y); };
        a.push_back({ 100, [] { showCaption("HP-41, HP-71 AND HP-75 FILES ON ONE DISK", 3500); } });
        a.push_back({ 3500, [row] { row(0); } });                               // programs
        a.push_back({ 4000, [] { showCaption("TOUCH A ROW: WHERE ARE THE PROGRAMS?", 3000); } });
        a.push_back({ 7500, [row] { row(0); } });                               // all again
        a.push_back({ 9000, [tab] { tab(true); } });                            // BY MACHINE
        a.push_back({ 9500, [] { showCaption("BY MACHINE", 2500); } });
        a.push_back({ 12000, [row] { row(1); } });                              // HP-71
        a.push_back({ 14500, [] {                                                // a secure + private file
            int x = 0, y = 0;
            if (mediamap_blockPoint(demoDiskFirstFile() + kDemoDisk[0].len / 2, x, y)) mediamap_tap(x, y);
        } });
        a.push_back({ 15000, [] { showCaption("TOUCH A FILE: BAS71 (SP) - SECURE + PRIVATE", 3500); } });
        a.push_back({ 20000, [tab] { tab(false); } });                          // (as it was)
        a.push_back({ 20100, [] { mediamap_clearDemoSource(); demoMap_ = false; } });
    });
}

// ── The printer's paper: pulled up to the tear-off edge, then torn off ──

Step paperPullUp() {
    return [n = 0, next = std::uint32_t(0)](bool first) mutable {
        if (first) { n = 0; next = nowMs(); }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        next = nowMs() + 250;
        if (n < 4) { plotterview_paperScrollBy(-1); ++n; return false; }   // (stops at the edge)
        return true;
    };
}

// ── A made-up HP-41 session (analyzer, loop map) ────────────────────────
// A whole made-up loop, one frame every 30 ms: two devices before HIPI,
// HIPI's own (hipi_simFrame()), three of a PC program behind PILBOX, two
// after HIPI. The HP-41 auto-addresses it, asks every device for its
// accessory and device ID, prints on the display and the printer, asks a
// few for their status, and reads the voltmeter. (Not every detail is as
// a real loop would have it -- it's for showing.)

void saveAddresses() {
    if (!savedAddr_.empty()) return;
    for (CDevice* d : devices) savedAddr_.push_back(d->addr());
}

void restoreAddresses() {
    for (std::size_t i = 0; i < savedAddr_.size() && i < devices.size(); ++i) devices[i]->addr(savedAddr_[i]);
    savedAddr_.clear();
}

Step session() {
    return [acts = std::vector<demo::LoopAct>(), i = std::size_t(0), phase = 0, last = IL_CMD_t(0), echoes = 0,
            next = std::uint32_t(0)](bool first) mutable {
        if (first) { phase = 0; i = 0; acts.clear(); echoes = 0; next = nowMs(); saveAddresses(); }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        next = nowMs() + 30;
        auto round = [](IL_CMD_t f) { return demo::fakeLoopRound(f, hipi_simFrame); };
        if (phase == 0) { last = round(IFC); phase = 1; return false; }
        if (phase == 1) { last = round(AAU); phase = 2; return false; }
        if (phase == 2) {
            last = round(AAD + 1);
            const int n = inAddrRange(last, AAD) ? static_cast<int>(last - AAD) - 1 : 0;
            auto addrOf = [](CDevice* d) { return d != nullptr && d->enabled() && d->addr() >= 1 && d->addr() <= 30 ? static_cast<int>(d->addr()) : 0; };
            acts = demo::fakeSessionActs(n, addrOf(videoDisplay), addrOf(printer));
            phase = 3;
            return false;
        }
        // An answer travels back round to the controller, and on round
        // the loop again, until the talker ends it (ETO)
        if (echoes > 0) {
            if (IS_DATA(last) && echoes < 40) { last = round(last); ++echoes; return false; }
            echoes = 0;
        }
        if (i >= acts.size()) return true;
        const demo::LoopAct a = acts[i++];
        last = round(a.frame);
        if (a.echo) echoes = 1;
        return false;
    };
}

// ── LEDs and pixels ─────────────────────────────────────────────────────

Step lightShow(std::uint32_t ms) {
    return [ms, until = std::uint32_t(0), next = std::uint32_t(0), k = 0](bool first) mutable {
        const bool leds = hwio::ledsFitted(), pixels = hwio::pixelsFitted();
        if (first) {
            until = nowMs() + ms;
            next = nowMs();
            k = 0;
            if (leds || pixels) { hwio::begin(); lights_ = true; }
        }
        if (!lights_) return true;
        if (static_cast<std::int32_t>(nowMs() - until) >= 0) {
            hwio::end();
            lights_ = false;
            return true;
        }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        next = nowMs() + 70;
        ++k;
        if (leds) for (int i = 0; i < 5; ++i) hwio::setLed(i, i == (k / 2) % 5);
        if (pixels) {
            const int n = std::min(hwio::pixelCount(), 16);
            for (int i = 0; i < n; ++i) {
                // A rainbow running along
                const double h = std::fmod((i * 360.0 / std::max(n, 6)) + k * 12.0, 360.0) / 60.0;
                const double x = 1 - std::fabs(std::fmod(h, 2.0) - 1);
                double r = 0, g = 0, b = 0;
                switch (static_cast<int>(h)) {
                    case 0: r = 1; g = x; break;
                    case 1: r = x; g = 1; break;
                    case 2: g = 1; b = x; break;
                    case 3: g = x; b = 1; break;
                    case 4: r = x; b = 1; break;
                    default: r = 1; b = x; break;
                }
                hwio::setPixel(i, static_cast<std::uint8_t>(r * 80), static_cast<std::uint8_t>(g * 80), static_cast<std::uint8_t>(b * 80));
            }
            hwio::showPixels();
        }
        return false;
    };
}

// ── The script ──────────────────────────────────────────────────────────

void add(Step s) { script_.push_back(std::move(s)); }

void buildScript() {
    script_.clear();
    const std::uint16_t kColours[] = { 0xFFFF, 0xFFE0, 0x07E0, 0x07FF, 0xF800 };
    const char* kColourNames[] = { "WHITE", "YELLOW", "GREEN", "CYAN", "RED" };

    // ── The display as an HP 82163 ──────────────────────────────────
    add(call([] { plotterview_setDisplayModel(false); }));
    add(view(DisplayOutput::Display, videoDisplay));
    add(call([] { screen->setTextSize(1); screen->setColumns(0); screen->setColor(0x07E0); }));
    add(caption(round_ == 0 ? "HIPI DEMO  -  touch to stop" : "HP 82163 VIDEO INTERFACE", 3500));
    add(type(Sink::Screen, demo::textIntro(), 25));
    add(wait(2500));
    add(type(Sink::Screen, demo::textEscapes(), 6));
    add(wait(3000));
    for (int s = 0; s <= 3; ++s) {
        add(call([s] { screen->setTextSize(static_cast<std::uint8_t>(s)); }));
        add(type(Sink::Screen, std::string("\x1b" "E") + "FONT SIZE " + std::to_string(s) + "\r\nHIPI", 30));
        add(wait(1300));
    }
    add(call([] { screen->setTextSize(1); }));
    add(caption("TEXT COLOURS", 2500));
    for (int c = 0; c < 5; ++c) {
        add(call([c, kColours] { screen->setColor(kColours[c]); }));
        add(type(Sink::Screen, std::string("TEXT COLOUR ") + kColourNames[c] + "\r\n", 20));
        add(wait(700));
    }
    add(call([] { screen->setColor(0x07E0); }));
    add(wait(800));

    // ── The same display as an HP 92198 ─────────────────────────────
    add(call([] {
        video80Term->setSmallFont(false);
        video80Term->setColor(0x07FF);
        video80Term->hardReset();
        plotterview_setDisplayModel(true);
    }));
    add(caption("OR AS AN HP 92198 - 80 COLUMNS", 3000));
    add(type(Sink::Video80, demo::text80(), 4));
    add(wait(4500));
    add(type(Sink::Video80, demo::text80Roman8(), 4));
    add(wait(3500));
    add(type(Sink::Video80, demo::text40(), 20));
    add(wait(3000));
    add(type(Sink::Video80, "\x1b[", 0));
    add(call([] { plotterview_setDisplayModel(false); }));

    // ── The cassette drive ──────────────────────────────────────────
    CDrive* drv = plotterview_drive();
    if (drv != nullptr && plotterview_isAvailable(DisplayOutput::Tape)) {
        add(view(DisplayOutput::Tape, drv));
        add(caption("HP 82161A CASSETTE DRIVE", 2500));
        add(wait(2600));
        add(tapePick());
        add(view(DisplayOutput::Display, videoDisplay));
        add(caption("DIR - WHAT'S ON THE CASSETTE", 3000));
        add(dir());
        add(wait(5000));
        add(mediaMap(8000));
        add(view(DisplayOutput::Tape, drv));
        add(wait(1500));
        add(createMedia());
        add(wait(1000));
    }

    add(machineMap());

    // ── The plotter ─────────────────────────────────────────────────
    add(call([] { plotterview_setPlotFit(false); plotterview_clearPlotter(); plotterview_setPaperBlack(true); }));
    add(view(DisplayOutput::Plotter, plotter));
    add(caption("HP 7470A PLOTTER", 2500));
    add(hpgl([] { return demo::hpglLogo(kLogoImage, kLogoImageWidth, kLogoImageHeight); }, 60));
    add(wait(2500));
    add(call([] { plotterview_clearPlotter(); plotterview_setPaperBlack(false); }));
    add(hpgl(demo::hpglSpirograph, 90));
    add(wait(800));
    add(caption("SCREENDUMP", 1500));
    add(wait(1600));
    add(call([] { showFlash(); }));
    add(wait(400));
    add(caption("SAVED AS A .BMP  (NOT IN THE DEMO)", 2500));
    add(wait(2600));
    // Fit: the plot fills the screen -- the axes slowly, a piece at a time, so
    // the view is seen growing with them; then the rest, faster
    add(call([] { plotterview_clearPlotter(); plotterview_setPlotFit(true); }));
    add(caption("PLOT VIEW: FIT", 2500));
    add(hpgl(demo::hpglFunctionAxes, 1));            // slowly: the view grows with them
    add(wait(800));
    add(hpgl(demo::hpglFunctionCurves, 25));
    add(wait(2000));
    // Digitizing on it: DP, a point picked, the plotter has it
    add(caption("DIGITIZING  (HP-GL DP)", 3000));
    add(hpgl([] { return std::string("DP;"); }, 3));
    add(wait(1500));
    add(digitizePoint());
    add(wait(1200));
    add(hpgl([] { return demo::hpglDigitizedMark(plotter->penX(), plotter->penY()); }, 4));
    add(wait(3500));
    add(call([] { plotterview_setPlotFit(false); }));
    add(call([] { plotterview_clearPlotter(); }));
    add(caption("THE HP-IL LOOP", 2500));
    add(hpgl(demo::hpglLoop, 60));
    add(wait(3000));

    // ── The printer ─────────────────────────────────────────────────
    if (printer != nullptr && plotterview_isAvailable(DisplayOutput::Printer)) {
        add(view(DisplayOutput::Printer, printer));
        add(caption("HP 82162A THERMAL PRINTER", 2500));
        add(wait(1500));
        add(print(450));
        add(wait(1500));
        add(caption("PULL THE PAPER UP TO THE TEAR-OFF EDGE", 2500));
        add(paperPullUp());
        add(wait(1500));
        add(caption("TEAR IT OFF - .BMP + .TXT  (NOT IN THE DEMO)", 3000));
        add(wait(1200));
        add(call([] { plotterview_tearOff(1); }));
        add(wait(2500));
    }

    // ── HP-IL: analyzer, loop map, signals ──────────────────────────
#if HIPI_ANALYZER
    add(call([] { analyzer_clear(); loopmap_reset(); }));
    add(view(DisplayOutput::Analyzer, nullptr));
    add(caption("HP-IL ANALYZER - A MADE-UP HP-41 SESSION", 3000));
    add(session());
    add(wait(3000));
    add(view(DisplayOutput::LoopMap, nullptr));
    add(caption("THE LOOP MAP - LEARNT FROM THE TRAFFIC", 3000));
    add(wait(4500));
    // A card touched: everything known about that device
    add(acts([](std::vector<Act>& a) {
        a.push_back({ 0, [] {
            int x = 0, y = 0;
            if (plotter != nullptr && loopmap_cardPoint(plotter->addr(), x, y)) loopmap_tap(x, y);
        } });
        a.push_back({ 300, [] { if (loopmap_detailsOpen()) showCaption("TOUCH A DEVICE: WHAT'S KNOWN ABOUT IT", 3500); } });
        a.push_back({ 5500, [] { loopmap_closeDetails(); } });
    }));
    add(wait(1500));
#endif
    add(view(DisplayOutput::Signals, nullptr));
    add(caption("HP-IL SIGNALS - LIKE AN OSCILLOSCOPE", 3000));
    add(wait(6000));

    // ── The clock (if the real-time clock is there and set) ─────────
    if (clock_available()) {
        add(call([] {
            clock_setShow(SaverShow::Clock, config.clockStyle() == 1);
            plotterview_showClock();
        }));
        add(caption("THE CLOCK - FROM THE REAL-TIME CLOCK", 3000));
        add(wait(6000));
    }

    // ── The hardware test page: LEDs, pixels, what's on the I2C bus ──
    add(view(DisplayOutput::HwTest, nullptr));
    add(caption("HARDWARE TEST - LEDS, PIXELS, I2C", 3000));
    add(wait(1500));
    add(call([] { hwtest_demoRun(); }));
    add(wait(4500));
    add(call([] { hwtest_showScan(); }));
    add(caption("SCAN I2C - EVERY ADDRESS ON THE BUS", 3000));
    add(wait(4000));

    // ── The end: name, lights, a screen saver ───────────────────────
    add(call([] { plotterview_clearPlotter(); plotterview_setPaperBlack(true); }));
    add(view(DisplayOutput::Plotter, plotter));
    add(hpgl([] { return demo::hpglLogo(kLogoImage, kLogoImageWidth, kLogoImageHeight); }, 400));
    add(call([] { showEndCard(); }));
    add(lightShow(6500));
    add(call([] { hideOverlay(); }));
    add(call([] {
        // Goose or rain, in turn (the clock had its own turn)
        clock_setShow(round_ % 2 == 0 ? SaverShow::Goose : SaverShow::Rain, config.clockStyle() == 1);
        plotterview_showClock();
    }));
    add(wait(9000));
    add(call([] { plotterview_leaveClock(); }));
}

// ── Putting everything back ─────────────────────────────────────────────

void restore() {
    if (pickerShown_ && hooks_.hidePicker) hooks_.hidePicker();
    pickerShown_ = false;
    if (createShown_ && hooks_.hideCreateMedia) hooks_.hideCreateMedia();
    createShown_ = false;
    hideOverlay();
    if (lights_) { hwio::end(); lights_ = false; }
    plotterview_setDemoTape(false);
    if (demoMap_) { mediamap_clearDemoSource(); demoMap_ = false; }
    if (tapeShown_) {
        CDrive* drv = plotterview_drive();
        plotterview_setTapeFile(drv != nullptr ? drv->mediaFile() : std::string());
        plotterview_setTapeEjected(drv != nullptr && drv->ejected());
        tapeShown_ = false;
    }
    restoreAddresses();
    // Settings as they are (only the screen had them changed)
    screen->setTextSize(config.fontSize());
    screen->setColor(config.textColor());
    screen->setColumns(config.columns());
    video80Term->setColor(config.textColor());
    video80Term->setSmallFont(config.v80Small());
    video80Term->setDefaultForty(config.v80Forty());
    plotterview_setPlotFit(config.plotFit());
    plotterview_setPaperBlack(config.plotPaperBlack());
    // Empty afterwards
    screen->clear();
    video80Term->hardReset();
    plotterview_clearPlotter();
    plotterview_clearPaper();
#if HIPI_ANALYZER
    analyzer_clear();
    loopmap_reset();
#endif
    plotterview_setDisplayModel(videoDisplay != nullptr && videoDisplay->isVideo80());
    // Back to where it started
    if (startOutput_ == DisplayOutput::Video80 || startOutput_ == DisplayOutput::Display)
        plotterview_showView(videoDisplay != nullptr && videoDisplay->isVideo80() ? DisplayOutput::Video80 : DisplayOutput::Display,
                             videoDisplay);
    else
        plotterview_showView(startOutput_, startDevice_);
}

}  // namespace

void demo_init(DisplayDriver* display, const DemoHooks& hooks, const char* version) {
    d_ = display;
    hooks_ = hooks;
    version_ = version != nullptr ? version : "";
}

bool demo_running() { return running_; }

void demo_start() {
    if (running_ || d_ == nullptr) return;
    LOGF("\r\n * Demo started");
    startOutput_ = plotterview_output();
    startDevice_ = plotterview_viewDevice();
    running_ = true;
    stopRequested_ = false;
    round_ = 0;
    waiting_ = gaveUp_ = false;
    buildScript();
    at_ = 0;
    first_ = true;
}

void demo_stop() {
    if (!running_) return;
    running_ = false;
    stopRequested_ = false;
    script_.clear();
    script_.shrink_to_fit();
    restore();
    backlight_activity();
    LOGF("\r\n * Demo stopped");
}

void demo_hpilActivity() {
    if (running_) stopRequested_ = true;
}

void demo_poll() {
    if (!running_) return;
    if (stopRequested_) { demo_stop(); return; }
    if (time_reached(nextActivity_)) {                 // no screen saver meanwhile
        nextActivity_ = make_timeout_time_ms(1000);
        backlight_activity();
    }
    // hipiview behind? Wait (see waiting_)
    const bool syncing = displayMirror_syncing();
    const bool behind = syncing || (first_ && displayMirror_backlog() > kCaughtUpBytes);
    if (!behind) gaveUp_ = false;
    if (behind && !gaveUp_) {
        if (!waiting_) { waiting_ = true; waitFrom_ = realMs(); }
        if (realMs() - waitFrom_ < (syncing ? kMaxSyncWaitMs : kMaxCatchUpMs)) return;
        gaveUp_ = true;                                // the viewer can't keep up: go on
    }
    if (waiting_) {
        pausedMs_ += realMs() - waitFrom_;
        waiting_ = false;
    }
    if (captionShown_ && captionDue()) hideOverlay();
    if (at_ >= script_.size()) {                       // round again
        ++round_;
        buildScript();
        at_ = 0;
        first_ = true;
        return;
    }
    if (script_[at_](first_)) {
        ++at_;
        first_ = true;
    } else {
        first_ = false;
    }
}


}  // namespace hipi
