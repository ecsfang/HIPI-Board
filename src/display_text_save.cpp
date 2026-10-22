// display_text_save.cpp -- the firmware's display text log (display_text.h):
// kept in the display's SDRAM, saved to the SD card
#define MODULE "TEXT"
#include "display_text.h"
#include "display_config.h"
#include "display_mirror.h"
#include "sd_paths.h"
#include "usb_msc.h"
#include "usb_serial.h"   // LOGF
#include "ff.h"
#include <cstdio>

namespace hipi {

namespace {

// 1 MB of SDRAM nobody shows; the viewer of the display mirror isn't sent
// any of it (displayMirror_quiet)
class SdramStore : public DisplayTextStore {
public:
    explicit SdramStore(LT7683* d) : d_(d) {}
    std::uint32_t capacity() const override { return LT7683::kTextLogBytes; }
    void write(std::uint32_t offset, const std::uint8_t* data, std::size_t len) override {
        displayMirror_quiet(true);
        d_->writeSdram(LT7683::kTextLogAddr + offset, data, len);
        displayMirror_quiet(false);
    }
    void read(std::uint32_t offset, std::uint8_t* data, std::size_t len) override {
        displayMirror_quiet(true);
        d_->readSdram(LT7683::kTextLogAddr + offset, data, len);
        displayMirror_quiet(false);
    }

private:
    LT7683* d_;
};

SdramStore* store_ = nullptr;
volatile bool pending_ = false;

constexpr unsigned kMaxTries = 10000;   // give up searching for a free name

}  // namespace

void displayText_init(LT7683* display) {
    static SdramStore store(display);
    store_ = &store;
    displayText().setStore(store_);
}

void displayText_request() { pending_ = true; }

bool displayText_takePending() {
    if (!pending_) return false;
    pending_ = false;
    return true;
}

bool displayText_save(std::string& message) {
    DisplayText& text = displayText();
    if (store_ == nullptr) {
        message = "No text log";
        return false;
    }
    if (text.empty()) {
        message = "No text to save";
        return false;
    }
    if (usbMscModeActive()) {
        message = "SD card in use by PC";
        return false;
    }
    // The first free "text/display_<n>.txt" (the folder is made if needed)
    f_mkdir(HIPI_DIR_TEXT);                 // FR_EXIST if it is -- fine
    char name[48];
    FILINFO info;
    for (unsigned n = 1;; ++n) {
        std::snprintf(name, sizeof(name), HIPI_DIR_TEXT "/display_%u.txt", n);
        if (f_stat(name, &info) == FR_NO_FILE) break;
        if (n >= kMaxTries) {
            message = "No free file name";
            return false;
        }
    }
    FIL file;
    if (f_open(&file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        message = "Can't create file";
        return false;
    }
    bool ok = true;
    std::uint32_t bytes = 0;
    text.read([&](const char* s, std::size_t len) {
        if (!ok) return;
        UINT bw = 0;
        ok = f_write(&file, s, static_cast<UINT>(len), &bw) == FR_OK && bw == len;
        bytes += static_cast<std::uint32_t>(len);
    });
    ok = (f_close(&file) == FR_OK) && ok;
    if (!ok) {
        f_unlink(name);
        message = "Write error";
        return false;
    }
    LOGF("\r\n * Display text saved: %s (%lu lines, %lu bytes%s)", name,
         static_cast<unsigned long>(text.lines()), static_cast<unsigned long>(bytes),
         text.overflowed() ? ", the oldest lines are gone" : "");
    message = name;
    return true;
}

}  // namespace hipi
