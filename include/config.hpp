// Config.hpp
//
// Small persistent settings holder. Values are stored as plain-text
// "key=value" lines on the SD card, so the file can be inspected or
// edited by hand if needed. Every setter rewrites the whole file, so
// the file on disk always matches the in-memory values.
#pragma once

#include "usb_serial.h"  // LOGF
#include "ff.h"
#include "usb_msc.h"     // usbMscModeActive(), for save()'s own guard
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cctype>
#include <map>

namespace hipi {

class Config {
public:
    static constexpr const char* kPath = "CONFIG.TXT";

    // Load values from the SD card. If the file doesn't exist yet (e.g.
    // first boot), the current defaults are kept and written out so a
    // valid file exists afterwards.
    void load() {
        FIL file;
        if (f_open(&file, kPath, FA_READ) != FR_OK) {
            LOGF("\r\n * Config: no %s yet, creating one with defaults", kPath);
            save();
            return;
        }

        // (512 bytes used to be enough -- with several drives and all the
        // newer settings the file is longer, and anything after byte 511
        // was silently ignored)
        static char buf[2048];
        UINT br = 0;
        f_read(&file, buf, sizeof(buf) - 1, &br);
        buf[br] = 0;
        f_close(&file);

        parse(buf);
        LOGF("\r\n * Config: loaded from %s", kPath);
    }

    // Rewrite the whole file with the current values. Called automatically
    // by every setter below -- you normally don't need to call this
    // yourself.
    void save() {
        // The SD card's raw blocks belong to the PC while "Connect to
        // PC" mode is active (see usb_msc.h) -- writing via FatFs here
        // at the same time would race the PC's own filesystem driver
        // over the same underlying storage. Settings changes made from
        // the menu during this window simply aren't persisted (the menu
        // itself, and every other setter below, still updates its own
        // in-memory value normally either way -- only the SD card write
        // is skipped), rather than risking that race.
        if (usbMscModeActive()) {
            LOGF("\r\n * Config: save skipped, SD card is connected to PC");
            return;
        }
        FIL file;
        if (f_open(&file, kPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
            LOGF("\r\n * Config: failed to save %s", kPath);
            return;
        }

        char line[160];
        UINT bw = 0;

        // One line per cassette drive: media.<device name>=<file>
        int n = 0;
        for (const auto& [dev, media] : driveMedia_) {
            n = std::snprintf(line, sizeof(line), "media.%s=%s\n", dev.c_str(), media.c_str());
            f_write(&file, line, static_cast<UINT>(n), &bw);
        }

        n = std::snprintf(line, sizeof(line), "textcolor=%u\n",
                           static_cast<unsigned>(textColor_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "trace=%d\n", trace_ ? 1 : 0);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "debug=%d\n", extTrace_ ? 1 : 0);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "fontsize=%u\n",
                           static_cast<unsigned>(fontSize_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "display_model=%s\n",
                           displayModel_ == 1 ? "92198" : "82163");
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "v80_font=%s\n", v80Small_ ? "small" : "large");
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "v80_columns=%d\n", v80Forty_ ? 40 : 80);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "hpil_leds=%s\nleds_fitted=%d\npixels_fitted=%d\n",
                           hpilLeds_ == 0 ? "off" : hpilLeds_ == 1 ? "all" : "messages",
                           ledsFitted_ ? 1 : 0, pixelsFitted_ ? 1 : 0);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "plotter_led=%s\n", plotterLed_ ? "plotter" : "off");
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "brightness=%u\n",
                           static_cast<unsigned>(brightness_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "drive_standby=%d\n", driveStandby_ ? 1 : 0);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "screendump_next=%u\n", screendumpNext_);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "columns=%u\n",
                           static_cast<unsigned>(columns_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "con_loop=%s\nplot_fit=%d\nplot_paper=%s\n",
                          conInternal_ ? "internal" : "cable", plotFit_ ? 1 : 0,
                          plotPaperBlack_ ? "black" : "white");
        f_write(&file, line, static_cast<UINT>(n), &bw);

        static const char* kCells[] = { "bands", "lines", "none" };
        n = std::snprintf(line, sizeof(line), "signal_cells=%s\n", kCells[signalCells_ < 3 ? signalCells_ : 0]);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "shortcut=%s\n",
                          kShortcutNames[shortcut_ < kShortcutCount ? shortcut_ : 0]);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        static const char* kMirror[] = { "off", "view", "control" };
        n = std::snprintf(line, sizeof(line), "display_mirror=%s\n", kMirror[displayMirror_ < 3 ? displayMirror_ : 0]);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "pixel_count=%u\npixel_order=%s\n", static_cast<unsigned>(pixelCount_),
                          pixelOrder_.c_str());
        f_write(&file, line, static_cast<UINT>(n), &bw);

        static const char* kPrtModes[] = { "man", "trace", "norm" };
        n = std::snprintf(line, sizeof(line), "printer_mode=%s\nprinter_intensity=%u\n",
                          kPrtModes[printerMode_ < 3 ? printerMode_ : 0],
                          static_cast<unsigned>(printerIntensity_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "saver=%u\nclock_style=%u\nclock_us=%d\nclock_12h=%d\nclock_fallback=%u\n",
                          static_cast<unsigned>(saverMode_), static_cast<unsigned>(clockStyle_),
                          clockUs_ ? 1 : 0, clock12h_ ? 1 : 0, static_cast<unsigned>(clockFallback_));
        f_write(&file, line, static_cast<UINT>(n), &bw);
        static const char* kClockFromPc[] = { "off", "auto", "ask" };
        n = std::snprintf(line, sizeof(line), "clock_from_pc=%s\n", kClockFromPc[clockFromPc_ < 3 ? clockFromPc_ : 1]);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "disabled_devices=%s\n",
                           disabledDevices_.c_str());
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "device_order=%s\n",
                           deviceOrder_.c_str());
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "known_devices=%s\n",
                           knownDevices_.c_str());
        f_write(&file, line, static_cast<UINT>(n), &bw);

        f_close(&file);
    }

    // ----- Accessors (each setter persists immediately) -----

    // Selected LIF (.dat) file of each cassette drive, by device name.
    // An old CONFIG.TXT's single "filename=" is used for TFDRIVE.
    std::string driveMedia(const std::string& device) const {
        const auto it = driveMedia_.find(device);
        if (it != driveMedia_.end()) return it->second;
        return device == "TFDRIVE" ? legacyFilename_ : std::string();
    }
    void setDriveMedia(const std::string& device, const std::string& f) {
        driveMedia_[device] = f;
        save();
    }

    std::uint16_t textColor() const { return textColor_; }
    void setTextColor(std::uint16_t c) { textColor_ = c; save(); }

    bool trace() const { return trace_; }
    void setTrace(bool t) { trace_ = t; save(); }

    bool extTrace() const { return extTrace_; }
    void setExtTrace(bool d) { extTrace_ = d; save(); }

    std::uint8_t fontSize() const { return fontSize_; }
    void setFontSize(std::uint8_t s) { fontSize_ = s; save(); }

    // TFDISPLAY's model: 0 = HP 82163 (Screen), 1 = HP 92198 / MC00701A
    // 80-column interface (Video80) -- Settings -> Screen -> Emulate
    std::uint8_t displayModel() const { return displayModel_; }
    void setDisplayModel(std::uint8_t m) { displayModel_ = m; save(); }
    // HP 92198: the small 8x16 font instead of 12x24 (7" panel), and
    // 40 x 20 instead of 80 x 24 after a reset
    bool v80Small() const { return v80Small_; }
    void setV80Small(bool s) { v80Small_ = s; save(); }
    bool v80Forty() const { return v80Forty_; }
    void setV80Forty(bool f) { v80Forty_ = f; save(); }

    // Hardware test page: HP-IL activity on LED 4 -- 0 off (it's free for
    // TFLEDS), 1 all frames, 2 messages (IDY ignored; leds.h) -- and
    // whether the LEDs / pixels are fitted (for now only used by the test
    // page)
    int hpilLeds() const { return hpilLeds_; }
    void setHpilLeds(int mode) { hpilLeds_ = static_cast<std::uint8_t>(mode); save(); }
    // ... and the plotter's ERROR light on LED 3 (else it's free for TFLEDS)
    bool plotterLed() const { return plotterLed_; }
    void setPlotterLed(bool on) { plotterLed_ = on; save(); }
    bool ledsFitted() const { return ledsFitted_; }
    void setLedsFitted(bool f) { ledsFitted_ = f; save(); }
    bool pixelsFitted() const { return pixelsFitted_; }
    void setPixelsFitted(bool f) { pixelsFitted_ = f; save(); }

    // TFDRIVE's OFF-STANDBY-ON switch: Off is stored as a disabled device
    // (disabled_devices); this flag tells Standby from On
    bool driveStandby() const { return driveStandby_; }
    const std::string& deviceOrder() const { return deviceOrder_; }
    void setDeviceOrder(const std::string& csv) { deviceOrder_ = csv; save(); }
    // Next screendump_<n>.bmp number to try (see screendump.cpp)
    unsigned screendumpNext() const { return screendumpNext_; }
    void setScreendumpNext(unsigned n) { screendumpNext_ = n; save(); }
    void setDriveStandby(bool s) { driveStandby_ = s; save(); }
    std::uint8_t brightness() const { return brightness_; }
    void setBrightness(std::uint8_t b) { brightness_ = b; save(); }

    // 0 = auto (max for current font size); see Screen::setColumns().
    std::uint8_t columns() const { return columns_; }
    void setColumns(std::uint8_t c) { columns_ = c; save(); }

    // Screen saver (Settings -> Screen saver): what happens after a while
    // without use, and how the clock looks
    enum SaverMode : std::uint8_t { SaverDim = 0, SaverClock = 1, SaverOff = 2, SaverRain = 3, SaverGoose = 4,
                                    SaverRandom = 5 };   // Random: one of the animations, random style
    static constexpr std::uint8_t kSaverModes = 6;
    // Clock mode without a usable clock (no RTC / time not set): this
    // screen saver instead -- SaverRain, SaverGoose or SaverDim
    std::uint8_t clockFallback() const { return clockFallback_; }
    void setClockFallback(std::uint8_t m) { clockFallback_ = m; save(); }
    std::uint8_t saverMode() const { return saverMode_; }
    void setSaverMode(std::uint8_t m) { saverMode_ = m; save(); }
    std::uint8_t clockStyle() const { return clockStyle_; }     // 0 dark, 1 LCD
    void setClockStyle(std::uint8_t s) { clockStyle_ = s; save(); }
    bool clockUs() const { return clockUs_; }                     // US date format
    void setClockUs(bool us) { clockUs_ = us; save(); }
    // Plotter view: fit the plot to the screen (else: the whole page)
    bool plotFit() const { return plotFit_; }
    void setPlotFit(bool f) { plotFit_ = f; save(); }
    bool plotPaperBlack() const { return plotPaperBlack_; }
    void setPlotPaperBlack(bool b) { plotPaperBlack_ = b; save(); }
    // 0 off, 1 view, 2 control (display_mirror.h: MirrorMode)
    std::uint8_t displayMirror() const { return displayMirror_; }
    void setDisplayMirror(std::uint8_t m) { displayMirror_ = m; save(); }
    // HP-IL signals view, bit cells: 0 bands, 1 lines, 2 none (hpil_diag.h)
    std::uint8_t signalCells() const { return signalCells_; }
    void setSignalCells(std::uint8_t c) { signalCells_ = c; save(); }
    // The left edge's shortcut (Settings -> Shortcut): a ShortcutView
    // (views.h), in that order -- shortcut=loopmap|analyzer|...|off
    static constexpr const char* kShortcutNames[] = { "loopmap", "analyzer", "signals", "chartable",
                                                      "hwtest", "off" };
    static constexpr std::uint8_t kShortcutCount = 6;
    std::uint8_t shortcut() const { return shortcut_; }
    void setShortcut(std::uint8_t v) { shortcut_ = v; save(); }
    // TFPIXEL: how many colour pixels are connected (told by the
    // controller, "0N<n>" -- see pixels.h)
    std::uint8_t pixelCount() const { return pixelCount_; }
    void setPixelCount(std::uint8_t n) { if (n == pixelCount_) return; pixelCount_ = n; save(); }
    // The colour byte order of the pixel strip: "RGB", "GRB", "BRG", "RBG",
    // "GBR" or "BGR" (pixels.h: kPixelOrders) -- Hardware test page
    const std::string& pixelOrder() const { return pixelOrder_; }
    void setPixelOrder(const std::string& o) { if (o == pixelOrder_) return; pixelOrder_ = o; save(); }
    // TFPRINT's MODE switch (0 MAN, 1 TRACE, 2 NORM) and INTENSITY
    // switch (0 LIGHT .. 4 DARK) -- see printer.h
    std::uint8_t printerMode() const { return printerMode_; }
    void setPrinterMode(std::uint8_t m) { if (m > 2 || m == printerMode_) return; printerMode_ = m; save(); }
    std::uint8_t printerIntensity() const { return printerIntensity_; }
    void setPrinterIntensity(std::uint8_t i) { if (i > 4 || i == printerIntensity_) return; printerIntensity_ = i; save(); }
    // Devices this CONFIG.TXT has seen. A device added in a newer version
    // isn't in it yet -- hipi_init() can then give it its first state
    // (the printer starts switched off) and add it.
    bool isKnownDevice(const std::string& name) const {
        return wrapCsv(knownDevices_).find(wrapCsv(name)) != std::string::npos;
    }
    // Adds a name not known yet (no save -- the caller saves once)
    void addKnownDevice(const std::string& name) {
        if (isKnownDevice(name)) return;
        if (!knownDevices_.empty()) knownDevices_ += ",";
        knownDevices_ += name;
    }
    // PILBox CON mode: the loop closed inside HIPI (no cable needed)
    bool conInternal() const { return conInternal_; }
    void setConInternal(bool in) { conInternal_ = in; save(); }
    // The clock chip set from hipiview's PC (mirror_clock.h): 0 off, 1 auto, 2 ask
    std::uint8_t clockFromPc() const { return clockFromPc_; }
    void setClockFromPc(std::uint8_t m) { clockFromPc_ = m; save(); }
    bool clock12h() const { return clock12h_; }                   // 12-hour time
    void setClock12h(bool h12) { clock12h_ = h12; save(); }

    // Sets trace_ and extTrace_ together with a single save() -- used by the
    // 3-state "Trace" menu (Off / On / Extended) so it doesn't write the
    // file twice for one selection.
    void setTraceMode(bool trace, bool debug) {
        trace_ = trace;
        extTrace_ = debug;
        save();
    }

    // Whether a given device name should start enabled. Devices not
    // mentioned in the stored list default to enabled -- only names
    // explicitly recorded as disabled are excluded.
    bool isDeviceEnabled(const std::string& name) const {
        return findDeviceToken(name) == std::string::npos;
    }

    // Records the given device's enabled/disabled state and persists it
    // immediately. Called from UiDialog's device-toggle callback (see
    // pico_main.cpp), matched back up against actual CDevice instances by
    // name() in hipi_init() at boot.
    void setDeviceEnabled(const std::string& name, bool enabled) {
        const bool currentlyEnabled = isDeviceEnabled(name);
        if (enabled == currentlyEnabled) return;   // no change, skip the save()

        if (!enabled) {
            // Add to the disabled list.
            if (!disabledDevices_.empty()) disabledDevices_ += ",";
            disabledDevices_ += name;
        } else {
            // Remove from the disabled list -- rebuild it without this name.
            std::string rebuilt;
            std::size_t start = 0;
            while (start <= disabledDevices_.size()) {
                std::size_t comma = disabledDevices_.find(',', start);
                const std::size_t tokenLen = (comma == std::string::npos)
                    ? std::string::npos : comma - start;
                std::string token = disabledDevices_.substr(start, tokenLen);
                if (!token.empty() && token != name) {
                    if (!rebuilt.empty()) rebuilt += ",";
                    rebuilt += token;
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            disabledDevices_ = rebuilt;
        }
        save();
    }

private:
    // Wraps in commas so a substring search can't false-positive on a
    // name that's merely a substring of another (e.g. "LED" inside
    // "TFLEDS") -- ",TFLEDS," only matches the exact token ",TFLEDS,".
    static std::string wrapCsv(const std::string& s) { return "," + s + ","; }

    std::size_t findDeviceToken(const std::string& name) const {
        return wrapCsv(disabledDevices_).find(wrapCsv(name));
    }

    // Very small "key=value" line parser. Unknown keys are ignored, so
    // old config files stay loadable as new keys get added later.
    void parse(const char* buf) {
        static char copy[2048];
        std::strncpy(copy, buf, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = 0;

        char* line = std::strtok(copy, "\r\n");
        while (line) {
            char* eq = std::strchr(line, '=');
            while (*line == ' ' || *line == '\t') ++line;
            if (eq && *line != '#') {               // ('#': a comment line)
                // Spaces around the key and the value are allowed
                char* keyEnd = eq;
                while (keyEnd > line && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) --keyEnd;
                *keyEnd = 0;
                *eq = 0;
                char* value = eq + 1;
                while (*value == ' ' || *value == '\t') ++value;
                for (char* e = value + std::strlen(value); e > value && (e[-1] == ' ' || e[-1] == '\t'); --e) e[-1] = 0;
                const char* key = line;
                if (std::strncmp(key, "media.", 6) == 0) {
                    driveMedia_[key + 6] = value;
                } else if (std::strcmp(key, "filename") == 0) {
                    legacyFilename_ = value;      // pre-multi-drive CONFIG.TXT
                } else if (std::strcmp(key, "textcolor") == 0) {
                    textColor_ = static_cast<std::uint16_t>(std::atoi(value));
                } else if (std::strcmp(key, "trace") == 0) {
                    trace_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "debug") == 0) {
                    extTrace_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "fontsize") == 0) {
                    fontSize_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "display_model") == 0) {
                    displayModel_ = std::strcmp(value, "92198") == 0 ? 1 : 0;
                } else if (std::strcmp(key, "v80_font") == 0) {
                    v80Small_ = std::strcmp(value, "small") == 0;
                } else if (std::strcmp(key, "v80_columns") == 0) {
                    v80Forty_ = std::atoi(value) == 40;
                } else if (std::strcmp(key, "hpil_leds") == 0) {
                    // off / all / messages (before 1.6.0: 0 / 1 -- 1 was all)
                    if (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0) hpilLeds_ = 0;
                    else if (std::strcmp(value, "all") == 0 || std::strcmp(value, "1") == 0) hpilLeds_ = 1;
                    else hpilLeds_ = 2;
                } else if (std::strcmp(key, "plotter_led") == 0) {
                    plotterLed_ = !(std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0);
                } else if (std::strcmp(key, "leds_fitted") == 0) {
                    ledsFitted_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "pixels_fitted") == 0) {
                    pixelsFitted_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "screendump_next") == 0) {
                    screendumpNext_ = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
                } else if (std::strcmp(key, "drive_standby") == 0) {
                    driveStandby_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "brightness") == 0) {
                    brightness_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "columns") == 0) {
                    columns_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "saver") == 0) {
                    saverMode_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "clock_style") == 0) {
                    clockStyle_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "clock_us") == 0) {
                    clockUs_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "plot_fit") == 0) {
                    plotFit_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "plot_paper") == 0) {
                    plotPaperBlack_ = std::strcmp(value, "black") == 0;
                } else if (std::strcmp(key, "shortcut") == 0) {
                    for (std::uint8_t i = 0; i < kShortcutCount; ++i)      // (unknown: stays the loop map)
                        if (std::strcmp(value, kShortcutNames[i]) == 0) shortcut_ = i;
                } else if (std::strcmp(key, "signal_cells") == 0) {
                    signalCells_ = std::strcmp(value, "lines") == 0 ? 1
                                 : std::strcmp(value, "none") == 0  ? 2 : 0;
                } else if (std::strcmp(key, "printer_mode") == 0) {
                    printerMode_ = std::strcmp(value, "trace") == 0 ? 1
                                 : std::strcmp(value, "norm") == 0  ? 2 : 0;
                } else if (std::strcmp(key, "printer_intensity") == 0) {
                    const int i = std::atoi(value);
                    if (i >= 0 && i <= 4) printerIntensity_ = static_cast<std::uint8_t>(i);
                } else if (std::strcmp(key, "known_devices") == 0) {
                    knownDevices_ = value;
                } else if (std::strcmp(key, "pixel_order") == 0) {
                    std::string v = value;
                    for (char& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (v.size() == 3 && v.find('R') != std::string::npos && v.find('G') != std::string::npos &&
                        v.find('B') != std::string::npos)
                        pixelOrder_ = v;
                } else if (std::strcmp(key, "pixel_count") == 0) {
                    const int n = std::atoi(value);
                    if (n >= 1 && n <= 255) pixelCount_ = static_cast<std::uint8_t>(n);
                } else if (std::strcmp(key, "display_mirror") == 0) {
                    // ("on": what View used to be called)
                    displayMirror_ = std::strcmp(value, "control") == 0 ? 2
                                   : (std::strcmp(value, "view") == 0 || std::strcmp(value, "on") == 0) ? 1 : 0;
                } else if (std::strcmp(key, "con_loop") == 0) {
                    conInternal_ = std::strcmp(value, "internal") == 0;
                } else if (std::strcmp(key, "clock_12h") == 0) {
                    clock12h_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "clock_from_pc") == 0) {
                    clockFromPc_ = std::strcmp(value, "off") == 0 ? 0 : std::strcmp(value, "ask") == 0 ? 2 : 1;
                } else if (std::strcmp(key, "clock_fallback") == 0) {
                    clockFallback_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "disabled_devices") == 0) {
                    disabledDevices_ = value;
                } else if (std::strcmp(key, "device_order") == 0) {
                    deviceOrder_ = value;
                }
            }
            line = std::strtok(nullptr, "\r\n");
        }
    }

    std::map<std::string, std::string> driveMedia_;   // device name -> .dat file
    std::string   legacyFilename_ = "";
    // Matches the FONT_COLOR default that was previously hardcoded in
    // pico_main.cpp, so first boot (before a config file exists) looks
    // the same as before.
    // Genuine RGB565 white. (Used to match the old hardcoded 8bpp
    // FONT_COLOR=0xFF value, but that's meaningless now that the display
    // runs real 16bpp -- 0x00FF there would render as a bluish tint, not
    // white.)
    std::uint16_t textColor_ = 0xFFFF;
    bool          trace_     = false;
    bool          extTrace_  = false;
    // Match the old hardcoded TEXT_SIZE/BRIGHTNESS constants from
    // pico_main.cpp, so first boot (before a config file exists) looks
    // the same as before.
    std::uint8_t  fontSize_   = 0;
    std::uint8_t  displayModel_ = 0;   // 82163
    bool          v80Small_   = false;
    bool          v80Forty_   = false;
    std::uint8_t  hpilLeds_   = 2;      // messages (leds.h kHpilLedMessages)
    bool          plotterLed_ = true;   // LED 3: the plotter's ERROR light
    bool          ledsFitted_ = true;
    bool          pixelsFitted_ = true;
    std::uint8_t  brightness_ = 0xFF;
    bool          driveStandby_ = false;
    unsigned      screendumpNext_ = 0;
    std::uint8_t  columns_    = 0;   // 0 = auto
    std::uint8_t  saverMode_  = 0;   // SaverDim
    std::uint8_t  clockStyle_ = 0;   // dark
    bool          clockUs_    = false;
    bool          clock12h_   = false;
    std::uint8_t  clockFromPc_ = 1;  // clock_from_pc=off|auto|ask (auto)
    bool          conInternal_ = false;   // con_loop=cable|internal
    bool          plotFit_     = true;    // plot_fit=1|0
    bool          plotPaperBlack_ = false; // plot_paper=white|black
    std::uint8_t  displayMirror_ = 0;      // display_mirror=off|view|control (USB CDC3, tools/hipiview)
    std::uint8_t  signalCells_ = 0;        // signal_cells=bands|lines|none
    std::uint8_t  shortcut_ = 0;           // shortcut=loopmap (kShortcutNames)
    std::uint8_t  pixelCount_ = 5;         // pixel_count=1..32 (TFPIXEL)
    std::string   pixelOrder_ = "RGB";     // pixel_order=RGB|GRB|BRG|RBG|GBR|BGR (the HIPI board's strip: RGB)
    std::uint8_t  printerMode_ = 0;        // printer_mode=man|trace|norm (TFPRINT)
    std::uint8_t  printerIntensity_ = 2;   // printer_intensity=0..4
    std::string   knownDevices_ = "";      // known_devices=<names> (see isKnownDevice())
    std::uint8_t  clockFallback_ = 3;  // SaverRain
    // Comma-separated device names (matched against CDevice::name()) that
    // should start disabled. Empty = everything enabled (the default).
    std::string   disabledDevices_ = "";
    // Loop order of the devices, comma-separated names (Devices -> Change
    // order). Empty = the default order they're created in (hipi_init()).
    std::string   deviceOrder_ = "";
};

}  // namespace hipi