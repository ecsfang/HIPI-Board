// mirror_download.h -- a drive's cassette saved on the PC: the LIF file
// of the drive whose Tape view is shown, sent to hipiview over the display
// mirror (mirror_protocol.h, FILES: kDownload), where the user chooses
// where to save it. Asked for from the Tape view's menu ("Save media on
// PC") or by hipiview (kDlAsk); View or Control.
//
// The file is read through the drive itself (CDrive::peekBlock(): its own
// open file, its position kept), a piece at a time and only while the drive
// is quiet -- an HP-IL command going on is never held up. If the drive
// writes to the cassette meanwhile, it starts over (kDlBegin again), so
// the PC always gets one consistent cassette. The stream is reliable
// already, so there are no acks: a piece goes out whenever the ring has
// room for it, the screen keeps the rest.
//
// No hardware here: the drive is behind DownloadDrive, so it runs on the
// PC too (tools/hipiview/test/download_test).
#pragma once
#include "mirror_protocol.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hipi {

class DownloadDrive {
public:
    virtual ~DownloadDrive() = default;
    virtual std::string name() const = 0;               // "TFDRIVE"
    virtual std::string mediaFile() const = 0;          // its cassette (in lif/), "" = none
    virtual bool quiet() const = 0;                     // no file access, no HP-IL message going on
    virtual std::uint32_t changes() const = 0;          // grows with every block written
    virtual std::uint32_t fileSize() const = 0;         // of the cassette's file, 0 = can't tell
    virtual bool readBlock(std::uint32_t block, std::uint8_t* out) = 0;   // 256 bytes
};

struct DownloadEnv {
    // The drive whose Tape view is shown; nullptr: none (another view)
    std::function<std::shared_ptr<DownloadDrive>()> drive;
    // Why it can't be done now ("" = it can): the SD card lent to the PC
    std::function<std::string()> notNow;
    std::function<std::uint64_t()> nowMs;
};

class MediaSender {
public:
    // A packet for the viewer: kind (kDlBegin ...) and its payload
    using Emit = std::function<void(std::uint8_t kind, const std::uint8_t* p, std::size_t n)>;
    // Room in the stream for a packet of n bytes now? (false: later)
    using Room = std::function<bool(std::size_t n)>;

    MediaSender(DownloadEnv env, Emit emit, Room room)
        : env_(std::move(env)), emit_(std::move(emit)), room_(std::move(room)) {}

    // Send the cassette of the drive shown. False (and why) if it can't:
    // then the viewer is told too (kDlError)
    bool start(std::string& why);
    // The viewer asked (kDlAsk) / said stop (kDlAbort)
    void ask() { std::string why; start(why); }
    void abort();
    // Main loop: the next pieces
    void poll();
    // The viewer is gone (its session ended): stop
    void viewerGone();

    bool busy() const { return state_ != State::Idle; }
    const std::string& file() const { return file_; }

    static constexpr std::uint64_t kStallMs = 30000;   // waiting for a busy drive this long: given up
    static constexpr int kMaxRestarts = 5;             // written to while sending: started over this often
    static constexpr int kPiecesPerPoll = 4;

private:
    enum class State : std::uint8_t { Idle, Waiting, Sending };

    void begin();
    void fail(const std::string& why);
    void text(std::uint8_t kind, const std::string& t);

    DownloadEnv env_;
    Emit emit_;
    Room room_;
    State state_ = State::Idle;
    std::shared_ptr<DownloadDrive> drive_;
    std::string file_;
    std::uint32_t size_ = 0, sent_ = 0, crc_ = 0, changes_ = 0;
    int restarts_ = 0;
    std::uint64_t progressAt_ = 0, waitSaid_ = 0;
};

}  // namespace hipi
