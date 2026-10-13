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
#include "analyzer.h"
#include "loopmap.h"
#include "clock.h"
#include "boardui.h"
#include "backlight.h"
#include "config.hpp"
#include "lif_info.hpp"
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
unsigned short hipi_simFrame(unsigned short frame);   // hipi.cpp
extern const std::uint16_t kLogoImageWidth, kLogoImageHeight;
extern const std::uint16_t kLogoImage[];

namespace hipi {

#ifdef DISPLAY_7INCH

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
    if (pickerShown_) return;                    // (the same overlay)
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

// ── A made-up HP-41 session (analyzer, loop map) ────────────────────────
// Through HIPI's own devices (hipi_simFrame()), one frame every 30 ms:
// auto-addressing, each device asked for its accessory and device ID,
// then a line printed on the display

void saveAddresses() {
    if (!savedAddr_.empty()) return;
    for (CDevice* d : devices) savedAddr_.push_back(d->addr());
}

void restoreAddresses() {
    for (std::size_t i = 0; i < savedAddr_.size() && i < devices.size(); ++i) devices[i]->addr(savedAddr_[i]);
    savedAddr_.clear();
}

Step session() {
    struct Act { IL_CMD_t frame; bool echo; };
    return [acts = std::vector<Act>(), i = std::size_t(0), phase = 0, last = IL_CMD_t(0), echoes = 0,
            next = std::uint32_t(0)](bool first) mutable {
        if (first) { phase = 0; i = 0; acts.clear(); echoes = 0; next = nowMs(); saveAddresses(); }
        if (static_cast<std::int32_t>(nowMs() - next) < 0) return false;
        next = nowMs() + 30;
        if (phase == 0) { last = hipi_simFrame(AAU); phase = 1; return false; }
        if (phase == 1) {
            last = hipi_simFrame(AAD + 1);
            const int n = inAddrRange(last, AAD) ? static_cast<int>(last - AAD) - 1 : 0;
            for (int a = 1; a <= n && a <= 30; ++a) {
                acts.push_back({ static_cast<IL_CMD_t>(TAD + a), false });
                acts.push_back({ SAI, true });
                acts.push_back({ UNT, false });
                acts.push_back({ static_cast<IL_CMD_t>(TAD + a), false });
                acts.push_back({ SDI, true });
                acts.push_back({ UNT, false });
            }
            if (videoDisplay != nullptr && videoDisplay->enabled() && videoDisplay->addr() >= 1 && videoDisplay->addr() <= 30) {
                acts.push_back({ static_cast<IL_CMD_t>(LAD + videoDisplay->addr()), false });
                for (const char* p = "HELLO FROM THE HP-41\r\n"; *p; ++p) acts.push_back({ static_cast<IL_CMD_t>(*p), false });
                acts.push_back({ UNL, false });
            }
            phase = 2;
            return false;
        }
        // An answer travels back round to the controller, and on round
        // the loop again, until the talker ends it (ETO)
        if (echoes > 0) {
            if (IS_DATA(last) && echoes < 40) { last = hipi_simFrame(last); ++echoes; return false; }
            echoes = 0;
        }
        if (i >= acts.size()) return true;
        const Act a = acts[i++];
        last = hipi_simFrame(a.frame);
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
    }

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
    add(call([] { plotterview_clearPlotter(); }));
    add(hpgl(demo::hpglFunctionPlot, 60));
    add(wait(2500));
    add(call([] { plotterview_clearPlotter(); }));
    add(caption("THE HP-IL LOOP", 2500));
    add(hpgl(demo::hpglLoop, 60));
    add(wait(3000));

    // ── The printer ─────────────────────────────────────────────────
    if (printer != nullptr && plotterview_isAvailable(DisplayOutput::Printer)) {
        add(view(DisplayOutput::Printer, printer));
        add(caption("HP 82162A THERMAL PRINTER", 2500));
        add(wait(1500));
        add(print(900));
        add(wait(2000));
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
    add(wait(6000));
#endif
    add(view(DisplayOutput::Signals, nullptr));
    add(caption("HP-IL SIGNALS - LIVE FROM THE CABLE", 3000));
    add(wait(5000));

    // ── The end: name, lights, a screen saver ───────────────────────
    add(call([] { plotterview_clearPlotter(); plotterview_setPaperBlack(true); }));
    add(view(DisplayOutput::Plotter, plotter));
    add(hpgl([] { return demo::hpglLogo(kLogoImage, kLogoImageWidth, kLogoImageHeight); }, 400));
    add(call([] { showEndCard(); }));
    add(lightShow(6500));
    add(call([] { hideOverlay(); }));
    add(call([] {
        // Rain, goose or the clock (if there is one), in turn
        const int kinds = clock_available() ? 3 : 2;
        const int k = round_ % kinds;
        clock_setShow(k == 0 ? SaverShow::Goose : k == 1 ? SaverShow::Rain : SaverShow::Clock,
                      config.clockStyle() == 1);
        plotterview_showClock();
    }));
    add(wait(9000));
    add(call([] { plotterview_leaveClock(); }));
}

// ── Putting everything back ─────────────────────────────────────────────

void restore() {
    if (pickerShown_ && hooks_.hidePicker) hooks_.hidePicker();
    pickerShown_ = false;
    hideOverlay();
    if (lights_) { hwio::end(); lights_ = false; }
    plotterview_setDemoTape(false);
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

#else   // 5" panel: no demo

void demo_init(DisplayDriver*, const DemoHooks&, const char*) {}
void demo_start() {}
void demo_stop() {}
bool demo_running() { return false; }
void demo_poll() {}
void demo_hpilActivity() {}

#endif

}  // namespace hipi
