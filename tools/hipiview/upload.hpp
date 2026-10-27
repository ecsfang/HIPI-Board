// upload.hpp -- hipiview's side of sending a file to HIPI, dropped on the
// window, for the drive whose Tape view is shown: a LIF volume (.dat /
// .lif) becomes its cassette; a single file (an HP-41 program, .raw ...:
// lif_file.h) is converted to a LIF file here and added to its cassette.
// The protocol: mirror_protocol.h (FILES); HIPI's side: mirror_upload.h.
//
//   start()    the file read and checked here first (a LIF volume, a name
//              HIPI takes / converted), then HIPI is asked (kUpAsk)
//   Confirm    HIPI said which drive and what's in it: question() is put to
//              the user (a message box in hipiview), answer() gives the reply
//   Sending    pieces of kUpChunk bytes, a few ahead of HIPI's last ack;
//              nothing heard for a second: again from the last ack
//   Ending     all acknowledged: kUpEnd (again every second) until HIPI
//              says it's in the drive (kUpDone) -- or what went wrong
// No SDL or serial port here: messages go out through `send` (the serial
// reader fills in the session byte), answers come in through onReply().
#pragma once
#include "../../include/lif_check.h"
#include "../../include/lif_file.h"
#include "../../include/mirror_protocol.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

class Uploader {
public:
    using Send = std::function<void(std::vector<std::uint8_t>)>;
    enum class Step { Idle, Asking, Confirm, Sending, Ending, Done, Failed };

    explicit Uploader(Send send) : send_(std::move(send)) {}

    Step step() const { return step_; }
    bool busy() const { return step_ != Step::Idle && step_ != Step::Done && step_ != Step::Failed; }
    const std::string& name() const { return name_; }
    // What to show: "sending GAMES.DAT  45%", the result, an error
    const std::string& status() const { return status_; }
    // Confirm: the question for the user, and the "yes" button's text
    const std::string& question() const { return question_; }
    const std::string& yesText() const { return yes_; }
    const std::string& noText() const { return no_; }
    // A single file for the cassette (else a whole cassette)
    bool isFile() const { return what_ == hipi::mirror::kUpFile; }

    // A file dropped: false (and why) if it can't be sent
    bool start(const std::string& path, double now, std::string& why) {
        if (busy()) { why = "a file is already on its way"; return false; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { why = "can't read " + path; return false; }
        data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        const std::size_t slash = path.find_last_of("/\\");
        return startWith(slash == std::string::npos ? path : path.substr(slash + 1), std::move(data_), now, why);
    }
    // The same with the bytes given (the tests)
    bool startWith(const std::string& name, std::vector<std::uint8_t> data, double now, std::string& why) {
        if (busy()) { why = "a file is already on its way"; return false; }
        pcName_ = name;
        what_ = hipi::mirror::kUpVolume;
        kind_.clear();
        if (hipi::lifIsSingleFile(data)) {
            // One LIF file with its directory entry (lifget's NAME.lif):
            // added as it is, its own name and type
            hipi::LifFileImage img;
            if (!hipi::lifFileFromLifget(data, img, why)) return false;
            what_ = hipi::mirror::kUpFile;
            kind_ = img.what;
            name_ = img.name;
            data_ = std::move(img.bytes);
        } else if (hipi::lifFileKind(name) != nullptr) {
            // A single file: as a LIF file (its directory entry, its blocks)
            hipi::LifFileImage img;
            if (!hipi::lifFileFromPc(name, data, img, why)) return false;
            what_ = hipi::mirror::kUpFile;
            kind_ = img.what;
            name_ = img.name;
            data_ = std::move(img.bytes);
        } else {
            data_ = std::move(data);
            name_ = name;
        }
        return startData(now, why);
    }

    // The user's answer to question()
    void answer(bool yes, double now) {
        if (step_ != Step::Confirm) return;
        if (!yes) {
            finish(Step::Failed, isFile() && replace_ ? "skipped -- " + name_ + " on " + media_ + " stays as it was"
                                                      : std::string("cancelled -- nothing changed on HIPI"));
            return;
        }
        step_ = Step::Sending;
        acked_ = next_ = 0;
        dupAcks_ = 0;
        progressAt_ = lastProgress_ = now;
        sendBegin();
        lastSend_ = now;
        status_ = "sending " + name_ + "  0%";
        pump(now);
    }

    // An answer from HIPI (kUpload)
    void onReply(std::uint8_t kind, const std::uint8_t* p, std::size_t n, double now) {
        const std::string text(reinterpret_cast<const char*>(p), n);
        switch (kind) {
        case hipi::mirror::kUpError:
            if (busy()) finish(Step::Failed, "HIPI: " + text);
            return;
        case hipi::mirror::kUpInfo: {
            if (step_ != Step::Asking || n < 1) return;
            const std::uint8_t flags = p[0];
            // "drive|cassette|new cassette" (older firmware: "drive|cassette")
            std::vector<std::string> f;
            for (std::size_t from = 1;;) {
                const std::size_t bar = text.find('|', from);
                f.push_back(text.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
            f.resize(3);
            drive_ = f[0];
            const std::string media = f[1];
            media_ = media;
            replace_ = false;
            no_ = "Cancel";
            if (isFile()) {
                // A file for the cassette: the PC's name, the cassette's if it differs
                const std::string what = name_ + " (" + kind_ + ", " + pcName_ + ")";
                if (media.empty()) {
                    question_ = drive_ + " is empty.\nPut " + what + " on a new cassette, " + f[2] + "?";
                    yes_ = "New cassette";
                } else if (flags & hipi::mirror::kUpExists) {
                    question_ = media + " in " + drive_ + " has a file " + name_ + " already.\nReplace it with " +
                                pcName_ + " (" + kind_ + ")?";
                    yes_ = "Replace";
                    no_ = "Skip";
                    replace_ = true;
                } else {
                    question_ = "Add " + what + " to " + media + " in " + drive_ + "?";
                    yes_ = "Add";
                }
                step_ = Step::Confirm;
                status_ = "waiting for your answer";
                return;
            }
            if (media.empty()) {
                question_ = "Put " + name_ + " into " + drive_ + "?\n" + drive_ + " is empty.";
                yes_ = "Put in";
            } else if (media == name_) {
                question_ = "Replace " + media + " in " + drive_ + " with the file dropped?";
                yes_ = "Replace";
            } else {
                question_ = "Replace " + media + " in " + drive_ + " with " + name_ + "?\n" + media +
                            " stays on the SD card.";
                yes_ = "Replace";
            }
            if (flags & hipi::mirror::kUpExists)
                question_ += "\nThe SD card's lif/" + name_ + " is overwritten.";
            step_ = Step::Confirm;
            status_ = "waiting for your answer";
            return;
        }
        case hipi::mirror::kUpAck: {
            if ((step_ != Step::Sending && step_ != Step::Ending) || n < 4) return;
            const std::uint64_t a = static_cast<std::uint64_t>(p[0] | (p[1] << 8) | (p[2] << 16)) |
                                    (static_cast<std::uint64_t>(p[3]) << 24);
            if (a > acked_ && a <= data_.size()) { acked_ = a; progressAt_ = lastProgress_ = now; dupAcks_ = 0; }
            else if (a == acked_ && next_ > acked_ && ++dupAcks_ >= 2) {
                // The same ack again while pieces after it are on their way:
                // one got lost -- from there again at once (not after a second)
                next_ = acked_;
                dupAcks_ = -static_cast<int>(kWindow / hipi::mirror::kUpChunk);   // (the rest's acks: ignored)
            }
            if (a < acked_) { acked_ = a; next_ = a; }      // (HIPI has less -- started over: from there)
            next_ = std::max(next_, acked_);
            if (step_ == Step::Sending) {
                pump(now);
                status_ = "sending " + name_ + "  " + std::to_string(percent()) + "%";
            }
            return;
        }
        case hipi::mirror::kUpWait:
            if (step_ == Step::Ending) { status_ = name_ + ": HIPI is " + text; progressAt_ = now; }
            return;
        case hipi::mirror::kUpDone:
            if (busy() && isFile()) {
                const std::size_t bar = text.find('|');
                finish(Step::Done, name_ + " is on " + (bar == std::string::npos ? media_ : text.substr(bar + 1)) +
                                       " in " + drive_ + " now");
            } else if (busy()) {
                finish(Step::Done, text + " is in " + drive_ + " now");
            }
            return;
        default:
            return;
        }
    }

    // Now and then (every frame): resends, giving up. linkUp false: the
    // picture is being sent again (a new session) -- the clocks stand still
    // meanwhile, nothing is given up for that
    void tick(double now, bool linkUp = true) {
        if (!linkUp) {
            const double d = lastTick_ > 0 ? now - lastTick_ : 0;
            startedAt_ += d; lastSend_ += d; progressAt_ += d; lastProgress_ += d;
            lastTick_ = now;
            return;
        }
        lastTick_ = now;
        switch (step_) {
        case Step::Asking:
            if (now - lastSend_ > 1.0) {
                if (now - startedAt_ > 5.0) {
                    finish(Step::Failed, "HIPI didn't answer -- is Mirror to PC set to Control, "
                                         "and is HIPI's firmware 1.6.0 or newer?");
                    return;
                }
                sendAsk();
                lastSend_ = now;
            }
            return;
        case Step::Sending:
            if (now - lastProgress_ > 15.0) { finish(Step::Failed, "HIPI stopped answering -- try again"); return; }
            if (now - progressAt_ > 0.5) {                 // nothing new for a while: again from the last ack
                // (Begin again too: HIPI says where it is -- or, if it lost
                // the file (a new session), starts over and says 0)
                sendBegin();
                next_ = acked_;
                lastSend_ = progressAt_ = now;
            }
            pump(now);
            if (acked_ == data_.size()) {
                step_ = Step::Ending;
                sendEnd();
                lastSend_ = progressAt_ = now;
                status_ = name_ + ": sent, HIPI checks it";
            }
            return;
        case Step::Ending:
            if (now - lastSend_ > 1.0) { sendEnd(); lastSend_ = now; }
            if (now - progressAt_ > 40.0) finish(Step::Failed, "no word from HIPI -- did it take " + name_ + "?");
            return;
        default:
            return;
        }
    }

    int percent() const {
        return data_.empty() ? 0 : static_cast<int>(acked_ * 100 / data_.size());
    }

    // An 'F' message (the session byte left 0: the serial reader fills it in)
    static std::vector<std::uint8_t> message(std::uint8_t kind, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> m = { hipi::mirror::kMsgFile, 0, kind,
                                        static_cast<std::uint8_t>(payload.size()),
                                        static_cast<std::uint8_t>(payload.size() >> 8) };
        m.insert(m.end(), payload.begin(), payload.end());
        const std::uint16_t crc = hipi::mirror::crc16(m.data() + 2, 3 + payload.size());
        m.push_back(static_cast<std::uint8_t>(crc));
        m.push_back(static_cast<std::uint8_t>(crc >> 8));
        return m;
    }

private:
    static constexpr std::uint64_t kWindow = 8 * hipi::mirror::kUpChunk;   // ahead of the last ack (HIPI queues 8)

    bool startData(double now, std::string& why) {
        if (isFile()) {
            if (data_.size() > (1u << 20)) { why = "too big for a cassette file"; return false; }
        } else {
            if (!hipi::lifVolumeNameOk(name_, why)) {
                why += " -- or a file HIPI can add to a cassette (.raw: an HP-41 program)";
                return false;
            }
            if (data_.size() > hipi::kLifLargestVolume) { why = "too big for a LIF volume"; return false; }
            if (!hipi::lifCheckVolume(data_.data(), std::min<std::size_t>(data_.size(), hipi::kLifBlock),
                                      data_.size(), why))
                return false;
        }
        crc_ = hipi::mirror::crc32(data_.data(), data_.size());
        step_ = Step::Asking;
        startedAt_ = lastSend_ = now;
        status_ = "asking HIPI about " + name_;
        question_.clear();
        sendAsk();
        return true;
    }

    static void put32(std::vector<std::uint8_t>& v, std::uint64_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    }
    void sendAsk() {
        std::vector<std::uint8_t> p = { what_ };
        put32(p, data_.size());
        p.insert(p.end(), name_.begin(), name_.end());
        send_(message(hipi::mirror::kUpAsk, p));
    }
    void sendBegin() {
        std::vector<std::uint8_t> p = { static_cast<std::uint8_t>(what_ | (replace_ ? hipi::mirror::kUpReplace : 0)) };
        put32(p, data_.size());
        put32(p, crc_);
        p.insert(p.end(), name_.begin(), name_.end());
        send_(message(hipi::mirror::kUpBegin, p));
    }
    void sendEnd() { send_(message(hipi::mirror::kUpEnd, {})); }
    // Pieces up to kWindow ahead of the last ack
    void pump(double) {
        while (next_ < data_.size() && next_ - acked_ < kWindow) {
            const std::size_t len = std::min<std::size_t>(hipi::mirror::kUpChunk, data_.size() - next_);
            std::vector<std::uint8_t> p;
            put32(p, next_);
            p.insert(p.end(), data_.begin() + static_cast<std::ptrdiff_t>(next_),
                     data_.begin() + static_cast<std::ptrdiff_t>(next_ + len));
            send_(message(hipi::mirror::kUpData, p));
            next_ += len;
        }
    }
    void finish(Step s, const std::string& text) {
        if (s == Step::Failed && (step_ == Step::Sending || step_ == Step::Ending))
            send_(message(hipi::mirror::kUpAbort, {}));
        step_ = s;
        status_ = text;
    }

    Send send_;
    Step step_ = Step::Idle;
    std::vector<std::uint8_t> data_;
    std::string name_, drive_, status_, question_, yes_, no_ = "Cancel";
    std::string pcName_, kind_, media_;         // the PC's file name, "HP-41 program", the cassette
    std::uint8_t what_ = hipi::mirror::kUpVolume;
    bool replace_ = false;                      // the file is on the cassette: the user said Replace
    std::uint32_t crc_ = 0;
    std::uint64_t acked_ = 0, next_ = 0;
    int dupAcks_ = 0;
    double startedAt_ = 0, lastSend_ = 0, progressAt_ = 0, lastProgress_ = 0, lastTick_ = 0;
};
