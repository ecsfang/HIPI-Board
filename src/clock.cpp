#define MODULE "CLOCK"
#include "clock.h"
#include "rtc.h"
#include "glyph_font.h"
#include "saver_anim.h"
#include "config.hpp"
#include "plotterview.h"
#include "touch.h"          // TOUCH_SDA/TOUCH_SCL, touch_i2c (the shared I2C bus)
#include "usb_serial.h"
#include "tusb.h"
#include "pico/time.h"
#include <cstdio>
#include <cstring>

extern hipi::Config config;

namespace hipi {

extern const GlyphFont kClockFontBig;      // src/clock_font_big.cpp
extern const GlyphFont kClockFontSmall;    // src/clock_font_small.cpp

namespace {

DisplayDriver* d_ = nullptr;
SaverShow show_ = SaverShow::Clock;     // see clock_setShow()
#if !HIPI_FAKE_RTC
CRtc* rtc_ = nullptr;
#endif

#if HIPI_FAKE_RTC
// ── Stand-in clock (no DS3231 yet) ──────────────────────────────────────
// Starts at the firmware's build time and counts with the Pico's timer;
// "time ..." on the console sets it. Lost at every restart.
std::int64_t fakeBase_ = 0;              // seconds since 2000-01-01 at fakeBaseUs_
std::uint64_t fakeBaseUs_ = 0;

// Days since 2000-01-01 <-> date (proleptic Gregorian)
std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468 - 10957;
}
void civilFromDays(std::int64_t z, int& y, int& m, int& d) {
    z += 719468 + 10957;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
}
void fakeSet(const DateTime& t) {
    fakeBase_ = daysFromCivil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    fakeBaseUs_ = time_us_64();
}
void fakeStartAtBuildTime() {
    // __DATE__ "Oct  3 2026", __TIME__ "07:04:05"
    static const char* kMon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {};
    DateTime t;
    std::sscanf(__DATE__, "%3s %d %d", mon, &t.day, &t.year);
    std::sscanf(__TIME__, "%d:%d:%d", &t.hour, &t.minute, &t.second);
    const char* p = std::strstr(kMon, mon);
    t.month = p ? static_cast<int>((p - kMon) / 3) + 1 : 1;
    fakeSet(t);
}
#endif

// The time source: the DS3231, or the stand-in above (HIPI_FAKE_RTC)
bool rtcPresent() {
#if HIPI_FAKE_RTC
    return true;
#else
    return rtc_ != nullptr && rtc_->present();
#endif
}
bool rtcValid() {
#if HIPI_FAKE_RTC
    return true;
#else
    return rtcPresent() && rtc_->valid();
#endif
}
bool rtcRead(DateTime& t) {
#if HIPI_FAKE_RTC
    const std::int64_t s = fakeBase_ + static_cast<std::int64_t>((time_us_64() - fakeBaseUs_) / 1000000);
    civilFromDays(s / 86400, t.year, t.month, t.day);
    const int sod = static_cast<int>(s % 86400);
    t.hour = sod / 3600;
    t.minute = (sod / 60) % 60;
    t.second = sod % 60;
    t.weekday = weekdayOf(t.year, t.month, t.day);
    return true;
#else
    return rtcPresent() && rtc_->read(t);
#endif
}
bool rtcSet(const DateTime& t) {
#if HIPI_FAKE_RTC
    fakeSet(t);
    return true;
#else
    return rtcPresent() && rtc_->set(t);
#endif
}
DateTime shown_;                 // what's on screen (minute resolution)
bool shownValid_ = false;
absolute_time_t nextRead_ = nil_time;
char consoleLine_[48];
int consoleLen_ = 0;

const char* kWeekdays[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
const char* kMonths[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                          "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };

// Styles: dark = white on black; LCD = dark segments on a light grey-green
// background, with the unlit segments faintly visible
struct Style { std::uint16_t fg, bg; };
Style style() {
    return config.clockStyle() == 1 ? Style{ 0x18C3, 0xBE35 } : Style{ 0xFFFF, 0x0000 };
}

bool visible() {
    return d_ != nullptr && plotterview_output() == DisplayOutput::Clock && !plotterview_isSplashVisible();
}

void draw(const DateTime* t) {
    const Style s = style();
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, s.bg);

    // Time, 24 h "14:05" or 12 h " 2:05" + "PM" (small, beside it)
    char timeText[8] = "--:--";
    const char* ampm = nullptr;
    if (t != nullptr) {
        int h = t->hour;
        if (config.clock12h()) {
            ampm = h < 12 ? "AM" : "PM";
            h %= 12;
            if (h == 0) h = 12;
            std::snprintf(timeText, sizeof(timeText), "%d:%02d", h, t->minute);   // centred as it is
        } else {
            std::snprintf(timeText, sizeof(timeText), "%02d:%02d", h, t->minute);
        }
    }
    const GlyphFont& big = kClockFontBig;
    const GlyphFont& small = kClockFontSmall;
    const int timeW = big.textWidth(timeText);
    const int ampmW = ampm ? small.textWidth(ampm) + 16 : 0;
    const int x0 = (SCREEN_MAX_X - timeW - ampmW) / 2;
    // Everything centred vertically: time, gap, date, gap, names
    const int blockH = big.height + 50 + small.height + 24 + small.height;
    const int timeY = (SCREEN_MAX_Y - blockH) / 2;
    drawGlyphText(d_, x0, timeY, big, timeText, s.fg, s.bg);
    if (ampm) drawGlyphText(d_, x0 + timeW + 16, timeY + 8, small, ampm, s.fg, s.bg);

    // Date, then weekday and month
    char dateText[16] = "";
    char nameText[32] = "SET THE CLOCK";
    if (t != nullptr) {
        if (config.clockUs())
            std::snprintf(dateText, sizeof(dateText), "%02d/%02d/%04d", t->month, t->day, t->year);
        else
            std::snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d", t->year, t->month, t->day);
        std::snprintf(nameText, sizeof(nameText), "%s  %s", kWeekdays[t->weekday % 7],
                      kMonths[(t->month + 11) % 12]);
    }
    const int dateY = timeY + big.height + 50;
    drawGlyphText(d_, (SCREEN_MAX_X - small.textWidth(dateText)) / 2, dateY, small, dateText, s.fg, s.bg);
    drawGlyphText(d_, (SCREEN_MAX_X - small.textWidth(nameText)) / 2, dateY + small.height + 24,
                  small, nameText, s.fg, s.bg);
}

// "time 2026-10-03 14:05[:30]"
bool parseAndSet(const char* arg, std::string& msg) {
    DateTime t;
    int n = std::sscanf(arg, "%d-%d-%d %d:%d:%d", &t.year, &t.month, &t.day, &t.hour, &t.minute, &t.second);
    if (n == 5) t.second = 0;
    if (n < 5 || t.year < 2000 || t.year > 2099 || t.month < 1 || t.month > 12 || t.day < 1 ||
        t.day > 31 || t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 || t.second < 0 ||
        t.second > 59) {
        msg = "use: time YYYY-MM-DD HH:MM[:SS]";
        return false;
    }
    if (!rtcPresent()) { msg = "no clock chip (DS3231) found"; return false; }
    if (!rtcSet(t)) { msg = "writing the clock failed"; return false; }
    shownValid_ = false;                     // redraw with the new time
    msg = "clock set: " + clock_nowText();
    return true;
}

void handleConsoleLine() {
    consoleLine_[consoleLen_] = '\0';
    const char* s = consoleLine_;
    while (*s == ' ') ++s;
    if (std::strncmp(s, "time", 4) != 0) return;
    s += 4;
    while (*s == ' ') ++s;
    std::string msg;
    if (*s == '\0') {
        msg = rtcPresent() ? (rtcValid() ? clock_nowText() : "clock not set")
                           : "no clock chip (DS3231) found";
    } else {
        parseAndSet(s, msg);
    }
    LOGF("\r\n * %s\r\n", msg.c_str());
}

void pollConsole() {
    while (tud_cdc_n_available(0) > 0) {
        const char c = static_cast<char>(tud_cdc_n_read_char(0));
        if (c == '\r' || c == '\n') {
            if (consoleLen_ > 0) handleConsoleLine();
            consoleLen_ = 0;
        } else if (consoleLen_ < static_cast<int>(sizeof(consoleLine_)) - 1) {
            consoleLine_[consoleLen_++] = c;
        }
    }
}

}  // namespace

void clock_init(DisplayDriver* display) {
    d_ = display;
#if HIPI_FAKE_RTC
    fakeStartAtBuildTime();
    LOGF("\r\n * Clock: NO RTC -- stand-in clock from the build time (HIPI_FAKE_RTC): %s",
         clock_nowText().c_str());
#else
    rtc_ = new CRtc(touch_i2c);
    if (rtc_->begin(TOUCH_SDA, TOUCH_SCL)) {
        LOGF("\r\n * Clock: DS3231 found, %s", rtc_->valid() ? clock_nowText().c_str() : "time not set");
    } else {
        LOGF("\r\n * Clock: no DS3231 found -- the clock screen saver isn't available");
    }
#endif
}

bool clock_available() {
    return rtcValid();
}

std::string clock_nowText() {
    DateTime t;
    if (!rtcRead(t)) return "";
    char b[40];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d %s", t.year, t.month, t.day, t.hour,
                  t.minute, t.second, kWeekdays[t.weekday % 7]);
    return b;
}

void clock_setShow(SaverShow show, bool lcd) {
    show_ = show;
    if (show == SaverShow::Rain)  anim_start(AnimKind::Rain, d_, lcd);
    if (show == SaverShow::Goose) anim_start(AnimKind::Goose, d_, lcd);
}

void clock_drawAll() {
    if (d_ == nullptr) return;
    if (show_ != SaverShow::Clock) {
        anim_drawAll();
        return;
    }
    DateTime t;
    const bool ok = clock_available() && rtcRead(t);
    draw(ok ? &t : nullptr);
    shown_ = t;
    shownValid_ = ok;
}

void clock_poll() {
    pollConsole();
    if (visible() && show_ != SaverShow::Clock) {
        anim_step();
        return;
    }
    if (!visible() || !time_reached(nextRead_)) return;
    nextRead_ = make_timeout_time_ms(1000);      // the RTC is read once a second
    DateTime t;
    if (!clock_available() || !rtcRead(t)) return;
    if (!shownValid_ || t.minute != shown_.minute || t.hour != shown_.hour || t.day != shown_.day) {
        draw(&t);
        shown_ = t;
        shownValid_ = true;
    }
}

}  // namespace hipi
