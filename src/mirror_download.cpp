// mirror_download.cpp -- see mirror_download.h
#define MODULE "DOWNLOAD"
#include "mirror_download.h"
#include "lif_check.h"
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
    state_ = State::Idle;
    drive_.reset();
    text(mirror::kDlError, why);
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
    LOGF("\r\n * Cassette to hipiview: %s from %s", file_.c_str(), drive_->name().c_str());
    poll();
    return true;
}

void MediaSender::abort() {
    if (!busy()) return;
    LOGF("\r\n * Cassette to hipiview: %s -- cancelled", file_.c_str());
    state_ = State::Idle;
    drive_.reset();
}

void MediaSender::viewerGone() {
    if (!busy()) return;
    LOGF("\r\n * Cassette to hipiview: %s -- the viewer went away", file_.c_str());
    state_ = State::Idle;
    drive_.reset();
}

// (Again) from the start
void MediaSender::begin() {
    size_ = drive_->fileSize();
    if (size_ == 0) { fail("can't read " + file_); return; }
    changes_ = drive_->changes();
    sent_ = 0;
    crc_ = 0;
    const std::string id = drive_->name() + "|" + file_;
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
    for (int k = 0; k < kPiecesPerPoll && sent_ < size_; ++k) {
        const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(mirror::kDlChunk), size_ - sent_);
        if (room_ && !room_(4 + 4 + n)) break;                // the stream is full: later
        const std::uint32_t first = sent_ / kLifBlock;
        const std::uint32_t blocks = (n + kLifBlock - 1) / kLifBlock;
        for (std::uint32_t b = 0; b < blocks; ++b)
            if (!drive_->readBlock(first + b, piece + 4 + b * kLifBlock)) { fail("can't read " + file_); return; }
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
    LOGF("\r\n * Cassette to hipiview: %s sent (%lu bytes)", file_.c_str(), static_cast<unsigned long>(size_));
    state_ = State::Idle;
    drive_.reset();
}

}  // namespace hipi
