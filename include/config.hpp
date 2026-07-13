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
            cdc0_printf("\r\n * Config: no %s yet, creating one with defaults", kPath);
            save();
            return;
        }

        char buf[512];
        UINT br = 0;
        f_read(&file, buf, sizeof(buf) - 1, &br);
        buf[br] = 0;
        f_close(&file);

        parse(buf);
        cdc0_printf("\r\n * Config: loaded from %s", kPath);
    }

    // Rewrite the whole file with the current values. Called automatically
    // by every setter below -- you normally don't need to call this
    // yourself.
    void save() {
        FIL file;
        if (f_open(&file, kPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
            cdc0_printf("\r\n * Config: failed to save %s", kPath);
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

        f_close(&file);
    }

    // ----- Accessors (each setter persists immediately) -----

    const std::string& filename() const { return filename_; }
    void setFilename(const std::string& f) { filename_ = f; save(); }

    std::uint16_t textColor() const { return textColor_; }
    void setTextColor(std::uint16_t c) { textColor_ = c; save(); }

    bool trace() const { return trace_; }
    void setTrace(bool t) { trace_ = t; save(); }

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
                }
            }
            line = std::strtok(nullptr, "\r\n");
        }
    }

    std::string   filename_  = "";
    // Matches the FONT_COLOR default that was previously hardcoded in
    // pico_main.cpp, so first boot (before a config file exists) looks
    // the same as before.
    std::uint16_t textColor_ = 0x00FF;
    bool          trace_     = false;
};

}  // namespace hp82163