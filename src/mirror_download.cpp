// mirror_download.cpp -- see mirror_download.h
#define MODULE "DOWNLOAD"
#include "mirror_download.h"
#include "lif_check.h"
#include "lif_dir.h"
#include "usb_serial.h"   // LOGF
#include <algorithm>

namespace hipi {

namespace {
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
}  // namespace

void MediaSender::text(std::uint8_t kind, const std::string& t) {
    emit_(kind, reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
}

void MediaSender::fail(const std::string& why) {
    LOGF("\r\n * Cassette to hipiview: %s", why.c_str());
    lastError_ = why;
    state_ = State::Idle;
    single_.clear();
    drive_.reset();
    text(mirror::kDlError, why);
}

bool MediaSender::startFile(const std::string& lifName, std::string& why) {
    if (busy()) { why = file_ + " is on its way to the PC already"; return false; }
    if (lifName.empty()) { why = "no file"; return false; }
    single_ = lifName;
    const bool ok = start(why);
    if (!ok) single_.clear();
    return ok;
}

bool MediaSender::start(std::string& why) {
    why.clear();
    if (busy()) { why = file_ + " is on its way to the PC already"; return false; }   // (that one goes on)
    if (env_.notNow) why = env_.notNow();
    std::shared_ptr<DownloadDrive> drive;
    if (why.empty()) {
        drive = env_.drive ? env_.drive() : nullptr;
        if (!drive) why = "show the Tape view of the drive first";
    }
    if (why.empty() && drive->mediaFile().empty()) why = drive->name() + " has no cassette";
    if (why.empty() && drive->fileSize() == 0) why = "can't read " + drive->mediaFile();
    if (why.empty() && drive->fileSize() > kLifLargestVolume) why = drive->mediaFile() + " is too big";
    if (!why.empty()) { fail(why); return false; }
    drive_ = drive;
    file_ = drive->mediaFile();
    state_ = State::Waiting;
    restarts_ = 0;
    progressAt_ = waitSaid_ = env_.nowMs();
    if (single_.empty())
        LOGF("\r\n * Cassette to hipiview: %s from %s", file_.c_str(), drive_->name().c_str());
    else
        LOGF("\r\n * File to hipiview: %s on %s in %s", single_.c_str(), file_.c_str(), drive_->name().c_str());
    lastError_.clear();
    poll();
    // (it may have failed at once: the file isn't there ...)
    if (!busy() && !lastError_.empty()) { why = lastError_; return false; }
    return true;
}

void MediaSender::abort() {
    if (!busy()) return;
    LOGF("\r\n * Cassette to hipiview: %s -- cancelled", file_.c_str());
    state_ = State::Idle;
    single_.clear();
    drive_.reset();
}

void MediaSender::viewerGone() {
    if (!busy()) return;
    LOGF("\r\n * Cassette to hipiview: %s -- the viewer went away", file_.c_str());
    state_ = State::Idle;
    single_.clear();
    drive_.reset();
}

// (Again) from the start
void MediaSender::begin() {
    changes_ = drive_->changes();
    head_.clear();
    std::string id;
    if (single_.empty()) {
        size_ = drive_->fileSize();
        if (size_ == 0) { fail("can't read " + file_); return; }
        firstBlock_ = 0;
        id = drive_->name() + "|" + file_;
    } else {
        // One file: where it is on the cassette, how long (as lifget -r)
        LifDir dir;
        std::string why;
        const LifReadBlock read = [this](std::uint32_t b, std::uint8_t* out) { return drive_->readBlock(b, out); };
        if (!lifReadDir(read, drive_->fileSize() / kLifBlock, dir, why)) { fail(file_ + ": " + why); return; }
        const LifDirEntry* e = dir.find(single_);
        if (e == nullptr) { fail(single_ + " isn't on " + file_ + " (any more)"); return; }
        size_ = lifFileLength(e->entry);
        firstBlock_ = e->start;
        if (size_ == 0) { fail(single_ + " is empty"); return; }
        // An HP-41 program: raw, as `lifget -r` (NAME.raw -- what V41,
        // decomp41 and the drag-in take). Anything else keeps its directory
        // entry, as `lifget` writes it (NAME.lif -- its type and lengths go
        // with it; lifput, or a drop on the Tape view, puts it back)
        if (e->type == 0xE080) {
            id = drive_->name() + "|" + single_ + ".raw|" + file_;
        } else {
            head_.assign(e->entry, e->entry + kLifEntrySize);
            size_ += kLifEntrySize;
            id = drive_->name() + "|" + single_ + ".lif|" + file_;
        }
    }
    sent_ = 0;
    crc_ = 0;
    std::uint8_t p[4 + 128];
    put32(p, size_);
    const std::size_t n = std::min<std::size_t>(id.size(), 128);
    std::copy(id.begin(), id.begin() + static_cast<std::ptrdiff_t>(n), p + 4);
    emit_(mirror::kDlBegin, p, 4 + n);
    state_ = State::Sending;
}

void MediaSender::poll() {
    if (state_ == State::Idle) return;
    const std::uint64_t now = env_.nowMs();
    if (drive_->mediaFile() != file_) { fail("the cassette in " + drive_->name() + " was changed"); return; }
    // Only between HP-IL messages: the drive's own file, its position kept
    if (!drive_->quiet()) {
        if (now - progressAt_ > kStallMs) { fail(drive_->name() + " stays busy -- try again later"); return; }
        if (state_ == State::Waiting && now - waitSaid_ >= 2000) {
            waitSaid_ = now;
            text(mirror::kDlWait, "waiting for " + drive_->name());
        }
        return;
    }
    if (state_ == State::Waiting) begin();
    if (state_ != State::Sending) return;
    if (drive_->changes() != changes_) {
        // Written to meanwhile: the PC gets the cassette as it is now
        if (++restarts_ > kMaxRestarts) { fail(drive_->name() + " keeps writing to " + file_ + " -- try again later"); return; }
        LOGF("\r\n * Cassette to hipiview: %s was written to -- from the start again", file_.c_str());
        begin();
        if (state_ != State::Sending) return;
    }
    static std::uint8_t piece[4 + mirror::kDlChunk];
    static std::uint8_t blocks[mirror::kDlChunk + 2 * kLifBlock];
    const std::uint32_t headLen = static_cast<std::uint32_t>(head_.size());
    for (int k = 0; k < kPiecesPerPoll && sent_ < size_; ++k) {
        const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(mirror::kDlChunk), size_ - sent_);
        if (room_ && !room_(4 + 4 + n)) break;                // the stream is full: later
        // The stream from sent_: the directory entry first (a .lif), then
        // the data from the medium
        std::uint32_t i = 0;
        for (; i < n && sent_ + i < headLen; ++i) piece[4 + i] = head_[sent_ + i];
        if (i < n) {
            const std::uint32_t d0 = sent_ + i - headLen, d1 = sent_ + n - headLen;     // data bytes [d0, d1)
            const std::uint32_t b0 = d0 / kLifBlock, b1 = (d1 + kLifBlock - 1) / kLifBlock;
            for (std::uint32_t b = b0; b < b1; ++b)
                if (!drive_->readBlock(firstBlock_ + b, blocks + (b - b0) * kLifBlock)) { fail("can't read " + file_); return; }
            std::copy(blocks + (d0 - b0 * kLifBlock), blocks + (d1 - b0 * kLifBlock), piece + 4 + i);
        }
        put32(piece, sent_);
        emit_(mirror::kDlData, piece, 4 + n);
        crc_ = mirror::crc32(piece + 4, n, crc_);
        sent_ += n;
        progressAt_ = now;
    }
    if (sent_ < size_) return;
    std::uint8_t c[4];
    put32(c, crc_);
    emit_(mirror::kDlEnd, c, sizeof c);
    if (single_.empty()) LOGF("\r\n * Cassette to hipiview: %s sent (%lu bytes)", file_.c_str(), static_cast<unsigned long>(size_));
    else LOGF("\r\n * File to hipiview: %s sent (%lu bytes)", single_.c_str(), static_cast<unsigned long>(size_));
    state_ = State::Idle;
    single_.clear();
    drive_.reset();
}

}  // namespace hipi
