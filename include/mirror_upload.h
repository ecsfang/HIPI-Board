// mirror_upload.h -- a file sent from hipiview to HIPI over the display
// mirror (mirror_protocol.h, FILES): today a LIF volume dropped on the Tape
// view, which goes into lif/ and into the drive shown -- as if its OPEN
// had been pressed and the new cassette put in.
//
// The steps (the viewer's side: tools/hipiview/upload.hpp):
//   kUpAsk    may it? Only with the Tape view of a drive shown, the SD card
//             not lent to the PC, no menu open, a sensible name (.dat /
//             .lif), the file not in another drive. The answer (kUpInfo)
//             says which drive and what's in it now, and whether lif/<name>
//             exists -- for the viewer's "Replace?" question.
//   kUpBegin  the user said yes: written to lif/.upload.tmp as it comes
//             (kUpData, in order, acknowledged)
//   kUpEnd    all there: CRC-32 and the LIF header checked. Then, once the
//             drive is quiet (no file access, no HP-IL message for a moment
//             -- an HP-IL command going on is never cut), the cassette goes
//             out, lif/<name> takes the new file and goes in: the drive's
//             file from now on (saved, as the file picker does).
// Anything wrong: kUpError, the temporary file deleted, the old cassette
// left as it was.
//
// Prepared for the next step -- a single file (an HP-41 program, .raw)
// added to the cassette that's in: kUpAsk/kUpBegin carry `what`
// (kUpVolume / kUpFile); kUpFile is refused for now.
//
// No hardware here: the drive is behind UploadDrive and the SD card is
// FatFs (ff.h), so it runs on the PC too (tools/hipiview/test/upload_test).
#pragma once
#include "mirror_protocol.h"
#include "ff.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hipi {

// The drive a file goes to
class UploadDrive {
public:
    virtual ~UploadDrive() = default;
    virtual std::string name() const = 0;               // "TFDRIVE"
    virtual std::string mediaFile() const = 0;          // its cassette (in lif/), "" = none
    virtual bool quiet() const = 0;                     // no file access, no HP-IL message going on
    virtual void open() = 0;                            // the cassette out, as with OPEN (the lid opens)
    virtual void insert(const std::string& file) = 0;   // lif/<file> in, lid closed, remembered
};

struct UploadEnv {
    // The drive whose Tape view is shown; nullptr: none (another view)
    std::function<std::shared_ptr<UploadDrive>()> drive;
    // Why it can't be done now ("" = it can): the SD card lent to the PC,
    // a menu open ...
    std::function<std::string()> notNow;
    // The drive (another one) that has lif/<file> in it, "" = none
    std::function<std::string(const std::string& file)> inOtherDrive;
    std::function<std::uint64_t()> nowMs;
};

class UploadReceiver {
public:
    // An answer for the viewer: kind (kUpInfo ...) and its payload
    using Reply = std::function<void(std::uint8_t kind, const std::uint8_t* p, std::size_t n)>;

    UploadReceiver(UploadEnv env, Reply reply) : env_(std::move(env)), reply_(std::move(reply)) {}
    ~UploadReceiver() { drop(); }

    // A message from the viewer (kUpAsk ...): its payload
    void handle(std::uint8_t kind, const std::uint8_t* p, std::size_t n);
    // Main loop: the cassette changed once the drive is quiet
    void poll();
    // The viewer is gone (its session ended): a file half-way is forgotten
    // (a finished one still goes in)
    void viewerGone();

    bool receiving() const { return state_ == State::Receiving; }
    bool waitingForDrive() const { return state_ == State::Swapping; }

    // How long the change waits for a busy drive before giving up
    static constexpr std::uint64_t kSwapTimeoutMs = 30000;
    static constexpr const char* kTempFile = "lif/.upload.tmp";

private:
    enum class State : std::uint8_t { Idle, Receiving, Swapping, Done };

    void info(const std::string& name, std::uint64_t size, std::uint8_t what);
    bool checkRequest(std::uint8_t what, const std::string& name, std::uint64_t size,
                      std::shared_ptr<UploadDrive>& drive);
    void error(const std::string& text);
    void ack();
    void text(std::uint8_t kind, const std::string& t);
    void finish();                         // kUpEnd: checks, then waits for the drive
    void swap();                           // the change itself
    void drop();                           // the temporary file gone, back to Idle

    UploadEnv env_;
    Reply reply_;
    State state_ = State::Idle;
    std::shared_ptr<UploadDrive> drive_;
    std::string name_;
    std::uint64_t size_ = 0, got_ = 0;
    std::uint32_t crc_ = 0, runningCrc_ = 0;
    FIL file_{};
    bool fileOpen_ = false;
    std::uint64_t swapSince_ = 0, waitSaid_ = 0;
};

}  // namespace hipi
