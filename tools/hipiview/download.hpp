// download.hpp -- hipiview's side of saving a drive's cassette on the PC:
// HIPI sends the LIF file of the drive whose Tape view is shown, in the
// stream (mirror_protocol.h, FILES: kDownload; HIPI's side:
// mirror_download.h). Started from HIPI's Tape view menu ("Save media on
// PC") -- it simply arrives -- or from here (the M key: ask()).
//
//   Asking     kDlAsk sent (again every second) until HIPI starts or says no
//   Receiving  kDlBegin came: the pieces, in order (the stream is reliable;
//              a kDlBegin again means "from the start": the drive wrote to
//              the cassette meanwhile)
//   Ready      kDlEnd, size and CRC-32 right: take() it and save it
// No SDL, no serial port, no files here (download_test runs it).
#pragma once
#include "../../include/mirror_protocol.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Downloader {
public:
    using Send = std::function<void(std::vector<std::uint8_t>)>;
    enum class Step { Idle, Asking, Receiving, Ready, Failed };

    explicit Downloader(Send send) : send_(std::move(send)) {}

    Step step() const { return step_; }
    bool busy() const { return step_ == Step::Asking || step_ == Step::Receiving; }
    // What to show: "receiving GAMES.DAT  45%", an error
    const std::string& status() const { return status_; }
    // Whether the user asked here (an error then gets a message box)
    bool askedHere() const { return askedHere_; }

    // The M key: the cassette of the drive HIPI shows, please
    void ask(double now) {
        if (busy()) return;
        askedHere_ = true;
        step_ = Step::Asking;
        askedAt_ = lastSend_ = lastData_ = now;
        status_ = "asking HIPI for the cassette";
        sendAsk();
    }
    // Stop (the window closes ...)
    void cancel() {
        if (busy()) send_(message(hipi::mirror::kDlAbort));
        step_ = Step::Idle;
    }

    // A kDownload packet from HIPI
    void onPacket(std::uint8_t kind, const std::uint8_t* p, std::size_t n, double now) {
        using namespace hipi::mirror;
        switch (kind) {
        case kDlBegin: {
            if (n < 4) return;
            // (from HIPI's menu it just arrives: a new one any time but while one comes)
            if (step_ != Step::Asking && step_ != Step::Receiving) askedHere_ = false;
            size_ = le32(p);
            const std::string id(reinterpret_cast<const char*>(p + 4), n - 4);
            const std::size_t bar = id.find('|');
            drive_ = id.substr(0, bar);
            name_ = bar == std::string::npos ? id : id.substr(bar + 1);
            data_.clear();
            data_.reserve(size_);
            if (step_ == Step::Receiving) ++restarts_;
            else restarts_ = 0;
            step_ = Step::Receiving;
            lastData_ = now;
            status_ = "receiving " + name_ + "  0%";
            return;
        }
        case kDlData: {
            if (step_ != Step::Receiving || n < 4) return;
            if (le32(p) != data_.size() || data_.size() + (n - 4) > size_) {
                fail("a piece of " + name_ + " went missing -- try again");
                return;
            }
            data_.insert(data_.end(), p + 4, p + n);
            lastData_ = now;
            status_ = "receiving " + name_ + "  " + std::to_string(percent()) + "%";
            return;
        }
        case kDlEnd: {
            if (step_ != Step::Receiving || n < 4) return;
            if (data_.size() != size_) { fail(name_ + " arrived short -- try again"); return; }
            if (crc32(data_.data(), data_.size()) != le32(p)) { fail(name_ + " arrived damaged (CRC) -- try again"); return; }
            step_ = Step::Ready;
            status_ = name_ + " from " + drive_ + ": where to save it?";
            return;
        }
        case kDlWait:
            // (HIPI has it in hand: no more asking, no "didn't answer")
            if (busy()) { status_ = "HIPI is " + std::string(reinterpret_cast<const char*>(p), n); lastData_ = askedAt_ = lastSend_ = now; }
            return;
        case kDlError:
            // (HIPI's menu couldn't send it: shown on HIPI itself, here only
            // in the title bar -- no message box)
            if (!busy()) askedHere_ = false;
            fail("HIPI: " + std::string(reinterpret_cast<const char*>(p), n));
            return;
        default:
            return;
        }
    }
    // The stream started over (a new session): what was coming is lost
    void onNewSession() {
        if (busy()) fail("the link to HIPI started over -- try again");
    }

    // Now and then (every frame): resends, giving up. linkUp false: the
    // picture is being sent again -- the clocks stand still meanwhile
    void tick(double now, bool linkUp = true) {
        if (!linkUp) {
            const double d = lastTick_ > 0 ? now - lastTick_ : 0;
            askedAt_ += d; lastSend_ += d; lastData_ += d;
            lastTick_ = now;
            return;
        }
        lastTick_ = now;
        if (step_ == Step::Asking) {
            if (now - askedAt_ > 5.0) {
                fail("HIPI didn't answer -- is Mirror to PC on, and is HIPI's firmware 1.6.0 or newer?");
                return;
            }
            if (now - lastSend_ > 1.0) { sendAsk(); lastSend_ = now; }
        } else if (step_ == Step::Receiving && now - lastData_ > 40.0) {
            fail("HIPI stopped sending " + name_ + " -- try again");
        }
    }

    // Ready: the file (its name on HIPI, the drive, the bytes); then Idle
    bool take(std::string& name, std::string& drive, std::vector<std::uint8_t>& data) {
        if (step_ != Step::Ready) return false;
        name = name_;
        drive = drive_;
        data.swap(data_);
        data_.clear();
        step_ = Step::Idle;
        return true;
    }

    int percent() const { return size_ == 0 ? 0 : static_cast<int>(data_.size() * 100 / size_); }
    int restarts() const { return restarts_; }

    // An 'F' message without payload (the session byte: the serial reader)
    static std::vector<std::uint8_t> message(std::uint8_t kind) {
        std::vector<std::uint8_t> m = { hipi::mirror::kMsgFile, 0, kind, 0, 0 };
        const std::uint16_t crc = hipi::mirror::crc16(m.data() + 2, 3);
        m.push_back(static_cast<std::uint8_t>(crc));
        m.push_back(static_cast<std::uint8_t>(crc >> 8));
        return m;
    }

private:
    static std::uint32_t le32(const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }
    void sendAsk() { send_(message(hipi::mirror::kDlAsk)); }
    void fail(const std::string& why) {
        step_ = Step::Failed;
        status_ = why;
        data_.clear();
    }

    Send send_;
    Step step_ = Step::Idle;
    bool askedHere_ = false;
    std::string name_, drive_, status_;
    std::uint32_t size_ = 0;
    std::vector<std::uint8_t> data_;
    int restarts_ = 0;
    double askedAt_ = 0, lastSend_ = 0, lastData_ = 0, lastTick_ = 0;
};
