// lif_create.cpp -- see lif_create.h
#include "lif_create.h"
#include "sd_paths.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace hipi {

const LifMediaType kLifMediaTypes[kLifMediaTypeCount] = {
    { "HP82161A cassette",   "CASS",   2, 1, 256,  64 },
    { "HP9114B disk",        "DISK",  77, 2,  16, 128 },
    { "HDRIVE1 640 kB disk", "DISK",  80, 2,  16, 128 },
    { "HDRIVE1 2 MB disk",   "DISK", 125, 1,  64, 512 },
    { "HDRIVE1 4 MB disk",   "DISK", 125, 2,  64, 512 },
    { "HDRIVE1 8 MB disk",   "DISK", 125, 4,  64, 512 },
    { "HDRIVE1 16 MB disk",  "DISK", 125, 8,  64, 512 },
};

namespace {

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kEntriesPerBlock = kBlock / 32;

void putBe(std::uint8_t* p, int len, std::uint32_t v) {
    for (int i = len - 1; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
}
std::uint8_t bcd(int v) {
    v = std::clamp(v, 0, 99);
    return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
}

}  // namespace

int lifDirBlocks(int entries) {
    return (entries + static_cast<int>(kEntriesPerBlock) - 1) / static_cast<int>(kEntriesPerBlock);
}

int lifMaxDirEntries(const LifMediaType& t) {
    return static_cast<int>(t.blocks() / 3 * kEntriesPerBlock);
}

std::vector<int> lifDirChoices(const LifMediaType& t) {
    std::vector<int> v;
    for (int n = 8; n <= lifMaxDirEntries(t); n *= 2) v.push_back(n);
    return v;
}

int lifDataBlocks(const LifMediaType& t, int entries) {
    return static_cast<int>(t.blocks()) - 2 - lifDirBlocks(entries);
}

void lifVolumeHeader(std::uint8_t b[256], const LifMediaType& t, int dirEntries,
                     const std::string& label, const LifDate* date) {
    std::memset(b, 0, 256);
    putBe(b + 0, 2, 0x8000);                       // LIF id
    for (int i = 0; i < 6; ++i)                    // volume label
        b[2 + i] = i < static_cast<int>(label.size()) ? static_cast<std::uint8_t>(label[static_cast<std::size_t>(i)]) : ' ';
    putBe(b + 8, 4, 2);                            // directory start block
    putBe(b + 16, 4, static_cast<std::uint32_t>(lifDirBlocks(dirEntries)));
    putBe(b + 20, 2, 1);                           // LIF version
    putBe(b + 24, 4, t.tracks);
    putBe(b + 28, 4, t.heads);
    putBe(b + 32, 4, t.sectors);
    if (date != nullptr) {                         // YY MM DD hh mm ss, BCD
        b[36] = bcd(date->year % 100);
        b[37] = bcd(date->month);
        b[38] = bcd(date->day);
        b[39] = bcd(date->hour);
        b[40] = bcd(date->minute);
        b[41] = bcd(date->second);
    }
}

// ── Names ───────────────────────────────────────────────────────────────

bool lifNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool lifNameValid(const std::string& name) {
    if (name.empty() || name.size() > static_cast<std::size_t>(kLifNameMax)) return false;
    return std::all_of(name.begin(), name.end(), lifNameChar);
}

std::string lifLabelFromName(const std::string& name) {
    std::string l = name.substr(0, 6);
    l.resize(6, ' ');
    return l;
}

std::string lifFilePath(const std::string& name) {
    return std::string(HIPI_DIR_LIF) + "/" + name + ".DAT";
}

bool lifFileExists(const std::string& name) {
    // FAT names don't depend on case: f_stat() finds "games.dat" for
    // "GAMES.DAT" too
    FILINFO info;
    return f_stat(lifFilePath(name).c_str(), &info) == FR_OK;
}

std::string lifSuggestName(const char* prefix) {
    char b[32];
    for (int n = 1; n <= 99; ++n) {
        std::snprintf(b, sizeof(b), "%s%02d", prefix, n);
        if (!lifFileExists(b)) return b;
    }
    return prefix;
}

std::uint64_t sdFreeBytes() {
    DWORD clusters = 0;
    FATFS* fs = nullptr;
    if (f_getfree("", &clusters, &fs) != FR_OK || fs == nullptr) return 0;
    return static_cast<std::uint64_t>(clusters) * fs->csize * 512u;
}

// ── Writing ─────────────────────────────────────────────────────────────

bool LifCreator::begin(const std::string& name, const LifMediaType& t, int dirEntries, const LifDate* date) {
    cancel();
    error_.clear();
    state_ = State::Failed;
    if (!lifNameValid(name)) { error_ = "Not a valid name"; return false; }
    if (lifFileExists(name)) { error_ = name + ".DAT already exists"; return false; }
    if (sdFreeBytes() < t.bytes() + 64 * 1024) { error_ = "Not enough space on the SD card"; return false; }
    f_mkdir(HIPI_DIR_LIF);                         // (FR_EXIST if it's there)
    path_ = lifFilePath(name);
    fileName_ = name + ".DAT";
    if (f_open(&file_, path_.c_str(), FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        error_ = "Can't create " + fileName_;
        return false;
    }
    open_ = true;
    total_ = t.blocks();
    dirBlocks_ = lifDirBlocks(dirEntries);
    buf_.assign(16 * kBlock, 0);
    lifVolumeHeader(buf_.data(), t, dirEntries, lifLabelFromName(name), date);
    UINT bw = 0;
    if (f_write(&file_, buf_.data(), kBlock, &bw) != FR_OK || bw != kBlock) {
        fail("Writing to the SD card failed");
        return false;
    }
    next_ = 1;
    state_ = State::Running;
    return true;
}

LifCreator::State LifCreator::step(std::uint32_t maxBytes) {
    if (state_ != State::Running) return state_;
    // Fill pattern of a block: 0xFF for the directory and the first data
    // block, zeros elsewhere (see lif_create.h)
    const std::uint32_t ffEnd = 2u + static_cast<std::uint32_t>(dirBlocks_) + 1u;   // [2, ffEnd)
    auto fillOf = [&](std::uint32_t blk) -> std::uint8_t { return (blk >= 2 && blk < ffEnd) ? 0xFF : 0x00; };
    std::uint32_t budget = std::max<std::uint32_t>(maxBytes / kBlock, 1);
    const std::uint32_t maxRun = static_cast<std::uint32_t>(buf_.size()) / kBlock;
    while (budget > 0 && next_ < total_) {
        // A run of blocks with the same fill, written at once
        const std::uint8_t fill = fillOf(next_);
        std::uint32_t run = 1;
        while (run < maxRun && run < budget && next_ + run < total_ && fillOf(next_ + run) == fill) ++run;
        std::fill(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(run * kBlock), fill);
        UINT bw = 0;
        if (f_write(&file_, buf_.data(), run * kBlock, &bw) != FR_OK || bw != run * kBlock) {
            fail("Writing to the SD card failed");
            return state_;
        }
        next_ += run;
        budget -= run;
    }
    if (next_ >= total_) {
        const bool ok = f_close(&file_) == FR_OK;
        open_ = false;
        buf_.clear();
        buf_.shrink_to_fit();
        if (!ok) { fail("Writing to the SD card failed"); return state_; }
        state_ = State::Done;
    }
    return state_;
}

void LifCreator::cancel() {
    if (state_ == State::Running) {
        if (open_) f_close(&file_);
        open_ = false;
        f_unlink(path_.c_str());
        state_ = State::Idle;
    }
    buf_.clear();
    buf_.shrink_to_fit();
}

void LifCreator::fail(const char* why) {
    if (open_) f_close(&file_);
    open_ = false;
    f_unlink(path_.c_str());
    error_ = why;
    state_ = State::Failed;
    buf_.clear();
    buf_.shrink_to_fit();
}

int LifCreator::percent() const {
    if (total_ == 0) return 0;
    return static_cast<int>(static_cast<std::uint64_t>(next_) * 100 / total_);
}

}  // namespace hipi
