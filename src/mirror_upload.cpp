// mirror_upload.cpp -- see mirror_upload.h
#define MODULE "UPLOAD"
#include "mirror_upload.h"
#include "lif_check.h"
#include "sd_paths.h"
#include "usb_serial.h"   // LOGF
#include <cctype>
#include <cstring>

namespace hipi {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool exists(const std::string& path) {
    FILINFO fi;
    return f_stat(path.c_str(), &fi) == FR_OK;
}

}  // namespace

void UploadReceiver::text(std::uint8_t kind, const std::string& t) {
    reply_(kind, reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
}

void UploadReceiver::error(const std::string& t) {
    LOGF("\r\n * File from hipiview: %s", t.c_str());
    drop();
    text(mirror::kUpError, t);
}

void UploadReceiver::ack() {
    const std::uint8_t p[4] = { static_cast<std::uint8_t>(got_), static_cast<std::uint8_t>(got_ >> 8),
                                static_cast<std::uint8_t>(got_ >> 16), static_cast<std::uint8_t>(got_ >> 24) };
    reply_(mirror::kUpAck, p, sizeof p);
}

void UploadReceiver::drop() {
    if (fileOpen_) { f_close(&file_); fileOpen_ = false; }
    if (state_ == State::Receiving || state_ == State::Swapping) f_unlink(kTempFile);
    state_ = State::Idle;
    drive_.reset();
}

bool UploadReceiver::checkRequest(std::uint8_t what, const std::string& name, std::uint64_t size,
                                  std::shared_ptr<UploadDrive>& drive) {
    std::string why;
    if (what != mirror::kUpVolume) { text(mirror::kUpError, "only LIF volumes (.dat / .lif) for now"); return false; }
    if (!lifVolumeNameOk(name, why)) { text(mirror::kUpError, why); return false; }
    if (size < 2 * kLifBlock || size > kLifLargestVolume) { text(mirror::kUpError, "not a LIF volume (its size)"); return false; }
    const std::string notNow = env_.notNow ? env_.notNow() : std::string();
    if (!notNow.empty()) { text(mirror::kUpError, notNow); return false; }
    drive = env_.drive ? env_.drive() : nullptr;
    if (!drive) { text(mirror::kUpError, "show the Tape view of the drive first"); return false; }
    const std::string other = env_.inOtherDrive ? env_.inOtherDrive(name) : std::string();
    if (!other.empty()) { text(mirror::kUpError, name + " is in " + other + " -- take it out there first"); return false; }
    return true;
}

void UploadReceiver::info(const std::string& name, std::uint64_t size, std::uint8_t what) {
    std::shared_ptr<UploadDrive> drive;
    if (state_ == State::Receiving || state_ == State::Swapping) {
        text(mirror::kUpError, "another file is on its way");
        return;
    }
    if (!checkRequest(what, name, size, drive)) return;
    const std::string media = drive->mediaFile();
    std::uint8_t flags = 0;
    if (!media.empty()) flags |= mirror::kUpHasMedia;
    if (exists(std::string(HIPI_DIR_LIF "/") + name)) flags |= mirror::kUpExists;
    const std::string t = std::string(1, static_cast<char>(flags)) + drive->name() + "|" + media;
    text(mirror::kUpInfo, t);
}

void UploadReceiver::handle(std::uint8_t kind, const std::uint8_t* p, std::size_t n) {
    switch (kind) {
    case mirror::kUpAsk: {
        if (n < 5) return;
        info(std::string(reinterpret_cast<const char*>(p + 5), n - 5), le32(p + 1), p[0]);
        return;
    }
    case mirror::kUpBegin: {
        if (n < 9) return;
        const std::uint8_t what = p[0];
        const std::uint64_t size = le32(p + 1);
        const std::uint32_t crc = le32(p + 5);
        const std::string name(reinterpret_cast<const char*>(p + 9), n - 9);
        // The same file again (an answer got lost): go on from where it is
        if ((state_ == State::Receiving || state_ == State::Swapping || state_ == State::Done) &&
            name == name_ && size == size_ && crc == crc_) {
            if (state_ == State::Receiving) ack();
            else if (state_ == State::Done) text(mirror::kUpDone, name_);
            return;
        }
        if (state_ == State::Receiving || state_ == State::Swapping) { text(mirror::kUpError, "another file is on its way"); return; }
        std::shared_ptr<UploadDrive> drive;
        if (!checkRequest(what, name, size, drive)) return;
        // Room for it
        DWORD clusters = 0;
        FATFS* fs = nullptr;
        if (f_getfree("", &clusters, &fs) == FR_OK && fs != nullptr &&
            static_cast<std::uint64_t>(clusters) * fs->csize * 512u < size + 64 * 1024) {
            text(mirror::kUpError, "not enough space on the SD card");
            return;
        }
        f_mkdir(HIPI_DIR_LIF);                     // (FR_EXIST if it's there)
        if (f_open(&file_, kTempFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
            text(mirror::kUpError, "can't write to the SD card");
            return;
        }
        fileOpen_ = true;
        state_ = State::Receiving;
        drive_ = drive;
        name_ = name;
        size_ = size;
        crc_ = crc;
        got_ = 0;
        runningCrc_ = 0;
        LOGF("\r\n * File from hipiview: %s (%lu bytes) for %s", name_.c_str(),
             static_cast<unsigned long>(size_), drive_->name().c_str());
        ack();
        return;
    }
    case mirror::kUpData: {
        if (state_ != State::Receiving) return;     // (late pieces of a finished or dropped file)
        if (n < 4) return;
        const std::uint64_t off = le32(p);
        const std::size_t len = n - 4;
        if (off == got_ && len > 0 && got_ + len <= size_) {
            UINT bw = 0;
            if (f_write(&file_, p + 4, static_cast<UINT>(len), &bw) != FR_OK || bw != len) {
                error("writing to the SD card failed");
                return;
            }
            runningCrc_ = mirror::crc32(p + 4, len, runningCrc_);
            got_ += len;
        }
        ack();                                      // (not the next piece: says where it is)
        return;
    }
    case mirror::kUpEnd:
        if (state_ == State::Done) { text(mirror::kUpDone, name_); return; }
        if (state_ == State::Swapping) { text(mirror::kUpWait, "waiting for " + drive_->name()); return; }
        if (state_ != State::Receiving) return;
        if (got_ < size_) { ack(); return; }        // (pieces still missing)
        finish();
        return;
    case mirror::kUpAbort:
        if (state_ == State::Receiving || state_ == State::Swapping) {
            LOGF("\r\n * File from hipiview: %s -- cancelled", name_.c_str());
            drop();
        }
        return;
    default:
        return;
    }
}

void UploadReceiver::finish() {
    if (fileOpen_) {
        if (f_close(&file_) != FR_OK) { fileOpen_ = false; error("writing to the SD card failed"); return; }
        fileOpen_ = false;
    }
    if (runningCrc_ != crc_) { error("the file arrived damaged (CRC) -- try again"); return; }
    // A LIF volume? (its first block, as written)
    std::uint8_t blk[kLifBlock];
    UINT br = 0;
    FIL f;
    if (f_open(&f, kTempFile, FA_READ) != FR_OK) { error("can't read it back from the SD card"); return; }
    const bool read = f_read(&f, blk, sizeof blk, &br) == FR_OK;
    f_close(&f);
    std::string why;
    if (!read || !lifCheckVolume(blk, br, size_, why)) { error(why.empty() ? "can't read it back" : why); return; }
    state_ = State::Swapping;
    swapSince_ = waitSaid_ = env_.nowMs();
    poll();                                         // (at once, if the drive is quiet)
    if (state_ == State::Swapping) text(mirror::kUpWait, "waiting for " + drive_->name());
}

void UploadReceiver::poll() {
    if (state_ != State::Swapping) return;
    const std::uint64_t now = env_.nowMs();
    if (!drive_->quiet()) {
        if (now - swapSince_ > kSwapTimeoutMs) { error(drive_->name() + " stays busy -- try again later"); return; }
        if (now - waitSaid_ >= 2000) { waitSaid_ = now; text(mirror::kUpWait, "waiting for " + drive_->name()); }
        return;
    }
    swap();
}

void UploadReceiver::swap() {
    // Another drive may have taken it meanwhile
    const std::string other = env_.inOtherDrive ? env_.inOtherDrive(name_) : std::string();
    if (!other.empty()) { error(name_ + " is in " + other + " -- take it out there first"); return; }
    drive_->open();                                 // the old cassette out (closed): its file is free
    const std::string path = std::string(HIPI_DIR_LIF "/") + name_;
    if (exists(path) && f_unlink(path.c_str()) != FR_OK) {
        drive_->insert(drive_->mediaFile());        // (as it was)
        error("can't replace " + name_ + " on the SD card");
        return;
    }
    if (f_rename(kTempFile, path.c_str()) != FR_OK) {
        drive_->insert(drive_->mediaFile());
        error("can't rename it on the SD card");
        return;
    }
    drive_->insert(name_);
    LOGF("\r\n * File from hipiview: %s is in %s now", name_.c_str(), drive_->name().c_str());
    state_ = State::Done;                           // (the file is ours now: nothing to delete)
    text(mirror::kUpDone, name_);
    drive_.reset();
}

void UploadReceiver::viewerGone() {
    if (state_ == State::Receiving) {
        LOGF("\r\n * File from hipiview: %s -- the viewer went away, dropped", name_.c_str());
        drop();
    }
}

}  // namespace hipi
