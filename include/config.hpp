// Config.hpp
//
// Small persistent settings holder. Values are stored as plain-text
// "key=value" lines on the SD card, so the file can be inspected or
// edited by hand if needed. Every setter rewrites the whole file, so
// the file on disk always matches the in-memory values.
#pragma once

#include "ff.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hp82163 {

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

        char buf[512];
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
        FIL file;
        if (f_open(&file, kPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
            LOGF("\r\n * Config: failed to save %s", kPath);
            return;
        }

        char line[160];
        UINT bw = 0;

        int n = std::snprintf(line, sizeof(line), "filename=%s\n", filename_.c_str());
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "textcolor=%u\n",
                           static_cast<unsigned>(textColor_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "trace=%d\n", trace_ ? 1 : 0);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "debug=%d\n", debug_ ? 1 : 0);
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "fontsize=%u\n",
                           static_cast<unsigned>(fontSize_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "brightness=%u\n",
                           static_cast<unsigned>(brightness_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        n = std::snprintf(line, sizeof(line), "columns=%u\n",
                           static_cast<unsigned>(columns_));
        f_write(&file, line, static_cast<UINT>(n), &bw);

        f_close(&file);
    }

    // ----- Accessors (each setter persists immediately) -----

    const std::string& filename() const { return filename_; }
    void setFilename(const std::string& f) { filename_ = f; save(); }

    std::uint16_t textColor() const { return textColor_; }
    void setTextColor(std::uint16_t c) { textColor_ = c; save(); }

    bool trace() const { return trace_; }
    void setTrace(bool t) { trace_ = t; save(); }

    bool debug() const { return debug_; }
    void setDebug(bool d) { debug_ = d; save(); }

    std::uint8_t fontSize() const { return fontSize_; }
    void setFontSize(std::uint8_t s) { fontSize_ = s; save(); }

    std::uint8_t brightness() const { return brightness_; }
    void setBrightness(std::uint8_t b) { brightness_ = b; save(); }

    // 0 = auto (max for current font size); see Screen::setColumns().
    std::uint8_t columns() const { return columns_; }
    void setColumns(std::uint8_t c) { columns_ = c; save(); }

    // Sets trace_ and debug_ together with a single save() -- used by the
    // 3-state "Trace" menu (Off / On / Extended) so it doesn't write the
    // file twice for one selection.
    void setTraceMode(bool trace, bool debug) {
        trace_ = trace;
        debug_ = debug;
        save();
    }

private:
    // Very small "key=value" line parser. Unknown keys are ignored, so
    // old config files stay loadable as new keys get added later.
    void parse(const char* buf) {
        char copy[512];
        std::strncpy(copy, buf, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = 0;

        char* line = std::strtok(copy, "\r\n");
        while (line) {
            char* eq = std::strchr(line, '=');
            if (eq) {
                *eq = 0;
                const char* key = line;
                const char* value = eq + 1;
                if (std::strcmp(key, "filename") == 0) {
                    filename_ = value;
                } else if (std::strcmp(key, "textcolor") == 0) {
                    textColor_ = static_cast<std::uint16_t>(std::atoi(value));
                } else if (std::strcmp(key, "trace") == 0) {
                    trace_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "debug") == 0) {
                    debug_ = std::atoi(value) != 0;
                } else if (std::strcmp(key, "fontsize") == 0) {
                    fontSize_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "brightness") == 0) {
                    brightness_ = static_cast<std::uint8_t>(std::atoi(value));
                } else if (std::strcmp(key, "columns") == 0) {
                    columns_ = static_cast<std::uint8_t>(std::atoi(value));
                }
            }
            line = std::strtok(nullptr, "\r\n");
        }
    }

    std::string   filename_  = "";
    // Matches the FONT_COLOR default that was previously hardcoded in
    // pico_main.cpp, so first boot (before a config file exists) looks
    // the same as before.
    // Genuine RGB565 white. (Used to match the old hardcoded 8bpp
    // FONT_COLOR=0xFF value, but that's meaningless now that the display
    // runs real 16bpp -- 0x00FF there would render as a bluish tint, not
    // white.)
    std::uint16_t textColor_ = 0xFFFF;
    bool          trace_     = false;
    bool          debug_     = false;
    // Match the old hardcoded TEXT_SIZE/BRIGHTNESS constants from
    // pico_main.cpp, so first boot (before a config file exists) looks
    // the same as before.
    std::uint8_t  fontSize_   = 0;
    std::uint8_t  brightness_ = 0xFF;
    std::uint8_t  columns_    = 0;   // 0 = auto
};

}  // namespace hp82163