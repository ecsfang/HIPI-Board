// display_mirror.cpp -- see display_mirror.h.
#define MODULE "MIRR"
#include "display_mirror.h"
#include "mirror_encoder.hpp"
#ifdef DISPLAY_7INCH
#include "mirror_session.hpp"
#endif
#include "usb_serial.h"
#include "ff.h"
#include "tusb.h"
#include "pico/time.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace hipi {

namespace {

constexpr std::uint8_t kItf = 3;                 // CDC3 (my_descriptors.c)
constexpr std::uint32_t kStallTimeoutMs = 1000;  // ring full this long: give up the session
// The viewer has gone quiet: data waits for its acknowledgement this long
// with not a word from it (hipiview suspended or hung with the port still
// open, the PC asleep). The session then ends and HIPI stops mirroring
// until the viewer asks again -- feeding a port nobody reads would only
// stall HIPI's main loop.
constexpr std::uint32_t kViewerGoneMs = 3000;
constexpr std::uint32_t kStallSilentMs = 500;    // ring full and the viewer silent this long: gone
// With nothing to send, an empty frame this often: the viewer knows the
// session is still there, and its acknowledgement shows HIPI the viewer is
// (see kViewerGoneMs)
constexpr std::uint32_t kHeartbeatMs = 1000;
constexpr std::size_t kDumpBytesPerPoll = 8192;  // SDRAM read-back per main loop pass

// ── Reliable link to the viewer ─────────────────────────────────────────
// USB occasionally loses a packet on its way to the PC. So the stream goes
// in numbered frames (mirror_protocol.h), and every frame stays in the ring
// buffer until the viewer has acknowledged it: the viewer acknowledges what
// it has received ('K'), and asks for a resend from where something went
// missing ('N'). Positions are byte counts in the ring's own stream (they
// only grow); the ring holds what's between `acked_` and `wr_`.
//
// Allocated when a session starts and freed when the viewer leaves, so
// the mirror costs no RAM otherwise.
constexpr std::size_t kRingSize = 32 * 1024;
std::uint8_t* ring_ = nullptr;
std::uint32_t wr_ = 0;      // written
std::uint32_t sent_ = 0;    // handed to USB
std::uint32_t acked_ = 0;   // acknowledged by the viewer
std::uint8_t sid_ = 0;      // session id (in every frame)
// Go-back-N timer: sent but not acknowledged for this long -- the last
// frame(s) got lost, and nothing after them shows the viewer the gap --
// send again from the last acknowledged position
constexpr std::uint32_t kResendAfterMs = 300;
absolute_time_t ackProgress_;
absolute_time_t lastHeard_;     // the viewer's last message (any)
absolute_time_t ringIdle_;      // when everything was last acknowledged
absolute_time_t lastWrite_;     // when the ring was last written

std::uint32_t ringFree() { return static_cast<std::uint32_t>(kRingSize) - (wr_ - acked_); }
[[maybe_unused]] void ringClear() {
    wr_ = sent_ = acked_ = 0;
    ackProgress_ = lastHeard_ = ringIdle_ = lastWrite_ = get_absolute_time();
}
std::int64_t msSince(absolute_time_t t) { return absolute_time_diff_us(t, get_absolute_time()) / 1000; }
[[maybe_unused]] bool ringAlloc() {
    if (ring_ == nullptr) ring_ = new (std::nothrow) std::uint8_t[kRingSize];
    ringClear();
    return ring_ != nullptr;
}
[[maybe_unused]] void ringFree_() { delete[] ring_; ring_ = nullptr; ringClear(); }

// Moves what the USB port can take now from the ring to it
void pumpUsb() {
    if (ring_ == nullptr || !tud_cdc_n_connected(kItf)) return;
    if (sent_ != acked_ &&
        absolute_time_diff_us(ackProgress_, get_absolute_time()) > static_cast<std::int64_t>(kResendAfterMs) * 1000) {
        sent_ = acked_;                       // no news from the viewer: send again
        tud_cdc_n_write_clear(kItf);
        ackProgress_ = get_absolute_time();
    }
    if (sent_ == wr_) return;
    // Sending starts after a quiet spell: the resend timer runs from now
    if (sent_ == acked_) ackProgress_ = get_absolute_time();
    bool wrote = false;
    while (sent_ != wr_) {
        const std::uint32_t room = tud_cdc_n_write_available(kItf);
        if (room == 0) break;
        const std::size_t at = sent_ % kRingSize;
        std::size_t n = std::min<std::size_t>(wr_ - sent_, kRingSize - at);
        n = std::min<std::size_t>(n, room);
        const std::uint32_t w = tud_cdc_n_write(kItf, ring_ + at, static_cast<std::uint32_t>(n));
        if (w == 0) break;
        sent_ += w;
        wrote = true;
    }
    if (wrote) tud_cdc_n_write_flush(kItf);
}

void ringWrite(const std::uint8_t* data, std::size_t n) {
    for (std::size_t i = 0; i < n; ) {
        const std::size_t at = wr_ % kRingSize;
        const std::size_t k = std::min(n - i, kRingSize - at);
        std::memcpy(ring_ + at, data + i, k);
        wr_ += static_cast<std::uint32_t>(k);
        i += k;
    }
    lastWrite_ = get_absolute_time();
}

// ── What the viewer sends ───────────────────────────────────────────────
// "HIPIVIEW" n id... : a (new) session, see mirror_protocol.h
// 'K' sid pos(4)     : everything before pos arrived
// 'N' sid pos(4)     : something after pos got lost -- send again from pos
// A session only starts on a request -- opening the port (DTR) alone isn't
// enough, so programs that probe serial ports (ModemManager, terminal
// programs) never set off a session.
constexpr char kRequest[] = "HIPIVIEW";
std::size_t requestMatch_ = 0;
enum class Req { Magic, Count, Ids, Ack, Input };
Req req_ = Req::Magic;
int idsWanted_ = 0, idBytes_ = 0;
std::uint32_t ids_[mirror::kMaxViewerAssets];
absolute_time_t reqDeadline_;
bool requestPending_ = false;      // a complete request arrived
std::uint8_t ackMsg_[6];
int ackLen_ = 0;
std::uint8_t inMsg_[8];               // 'T' / 'B' from the viewer
std::size_t inLen_ = 0, inWant_ = 0;

MirrorMode mode_ = MirrorMode::Off;   // the setting (config display_mirror)
MirrorTouchHandler touchHandler_;
MirrorButtonHandler buttonHandler_;
bool inputHeld_ = false;              // the viewer has a finger/button down
bool inputSaid_ = false;              // input logged once for this session

// Input is queued here and handed to the handlers only from the top of
// displayMirror_poll() (deliverInput()). serviceRx() also runs while the
// ring is full -- from RingSink::put(), i.e. in the middle of some drawing
// -- and a handler that draws (a button press opens a menu...) must never
// run in there: it would draw into a half-done drawing and, through
// put() again, nest without end. (It did: HIPI crashed.)
struct QueuedInput { char kind; std::uint8_t code, down; std::uint16_t x, y; };   // kind 'T', 'B', 'R' (release all)
constexpr int kInputQueue = 16;
QueuedInput inQueue_[kInputQueue];
int inQueueHead_ = 0, inQueueCount_ = 0;

void queueInput(const QueuedInput& q) {
    if (inQueueCount_ == kInputQueue) {           // full: the oldest goes (repeats come anyway)
        inQueueHead_ = (inQueueHead_ + 1) % kInputQueue;
        --inQueueCount_;
    }
    inQueue_[(inQueueHead_ + inQueueCount_) % kInputQueue] = q;
    ++inQueueCount_;
}

void deliverInput() {
    while (inQueueCount_ > 0) {
        const QueuedInput q = inQueue_[inQueueHead_];
        inQueueHead_ = (inQueueHead_ + 1) % kInputQueue;
        --inQueueCount_;
        if (q.kind == 'T') {
            if (touchHandler_) touchHandler_(q.down != 0, q.x, q.y);
        } else if (q.kind == 'B') {
            if (buttonHandler_) buttonHandler_(q.code, q.down != 0);
        } else {
            if (touchHandler_) touchHandler_(false, 0, 0);
            if (buttonHandler_) buttonHandler_(0, false);
        }
    }
}

// Lets go of whatever the viewer holds (session over, Control off)
void releaseInput() {
    if (!inputHeld_) return;
    inputHeld_ = false;
    inQueueCount_ = 0;                            // what's still waiting doesn't count any more
    queueInput({ 'R', 0, 0, 0, 0 });
}

void handleInput(const std::uint8_t* m) {
    // Only in Control, and only from the viewer of the running session
    if (ring_ == nullptr || m[1] != sid_) return;
    if (mode_ != MirrorMode::Control) {
        if (!inputSaid_) {
            LOGF("\r\n * Display mirror: mouse/keys from the viewer ignored -- "
                 "Mirror to PC is %s (Control lets them through)", displayMirror_modeName(mode_));
            inputSaid_ = true;
        }
        return;
    }
    if (!inputSaid_) {
        LOGF("\r\n * Display mirror: the viewer is using the mouse/keys");
        inputSaid_ = true;
    }
    if (m[0] == mirror::kMsgTouch) {
        const bool down = m[6] != 0;
        const std::uint16_t x = static_cast<std::uint16_t>(m[2] | (m[3] << 8));
        const std::uint16_t y = static_cast<std::uint16_t>(m[4] | (m[5] << 8));
        if (down) inputHeld_ = true;
        queueInput({ 'T', 0, static_cast<std::uint8_t>(down), x, y });
    } else {
        if (m[3] != 0) inputHeld_ = true;
        queueInput({ 'B', m[2], m[3], 0, 0 });
    }
}

void handleAck(const std::uint8_t* m) {
    if (ring_ == nullptr || m[1] != sid_) return;          // another session's
    lastHeard_ = get_absolute_time();
    const std::uint32_t pos = static_cast<std::uint32_t>(m[2] | (m[3] << 8) | (m[4] << 16)) |
                              (static_cast<std::uint32_t>(m[5]) << 24);
    // Only positions we have actually sent count
    if (static_cast<std::int32_t>(pos - acked_) < 0 || static_cast<std::int32_t>(sent_ - pos) < 0) return;
    if (pos != acked_ || m[0] == 'N') ackProgress_ = get_absolute_time();
    acked_ = pos;
    if (m[0] == 'N') {
        // Resend from pos: drop what's still queued in the USB port (the
        // viewer throws away anything that doesn't fit anyway)
        sent_ = pos;
        tud_cdc_n_write_clear(kItf);
    }
}

// Reads and handles everything the viewer sent
void serviceRx() {
    std::uint8_t buf[64];
    while (tud_cdc_n_available(kItf) > 0) {
        const std::uint32_t n = tud_cdc_n_read(kItf, buf, sizeof buf);
        if (n == 0) break;
        lastHeard_ = get_absolute_time();
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint8_t b = buf[i];
            switch (req_) {
            case Req::Magic:
                if (requestMatch_ == 0 && (b == 'K' || b == 'N')) {
                    ackMsg_[0] = b;
                    ackLen_ = 1;
                    req_ = Req::Ack;
                } else if (requestMatch_ == 0 && (b == mirror::kMsgTouch || b == mirror::kMsgButton)) {
                    inMsg_[0] = b;
                    inLen_ = 1;
                    inWant_ = b == mirror::kMsgTouch ? mirror::kMsgTouchLen : mirror::kMsgButtonLen;
                    req_ = Req::Input;
                } else if (b == static_cast<std::uint8_t>(kRequest[requestMatch_])) {
                    if (++requestMatch_ == sizeof(kRequest) - 1) {
                        requestMatch_ = 0;
                        req_ = Req::Count;
                        reqDeadline_ = make_timeout_time_ms(300);
                    }
                } else {
                    requestMatch_ = b == static_cast<std::uint8_t>(kRequest[0]) ? 1 : 0;
                }
                break;
            case Req::Ack:
                ackMsg_[ackLen_++] = b;
                if (ackLen_ == 6) { handleAck(ackMsg_); req_ = Req::Magic; }
                break;
            case Req::Input:
                inMsg_[inLen_++] = b;
                if (inLen_ == inWant_) { handleInput(inMsg_); req_ = Req::Magic; }
                break;
            case Req::Count:
                idsWanted_ = std::min<int>(b, mirror::kMaxViewerAssets);
                idBytes_ = 0;
                req_ = idsWanted_ > 0 ? Req::Ids : Req::Magic;
                if (idsWanted_ == 0) requestPending_ = true;
                break;
            case Req::Ids: {
                const int k = idBytes_ / 4, sh = (idBytes_ % 4) * 8;
                if (sh == 0) ids_[k] = 0;
                ids_[k] |= static_cast<std::uint32_t>(b) << sh;
                if (++idBytes_ == idsWanted_ * 4) { req_ = Req::Magic; requestPending_ = true; }
                break;
            }
            }
        }
    }
    // An older viewer that sends only "HIPIVIEW": go ahead without ids
    if (req_ == Req::Count && time_reached(reqDeadline_)) {
        req_ = Req::Magic;
        idsWanted_ = 0;
        requestPending_ = true;
    }
}

// What the encoder hands over goes in numbered frames of at most
// kMaxFramePayload bytes: small frames, so a lost USB packet costs little
// to send again (and a long frame isn't likely to be hit every time)
constexpr std::size_t kMaxFramePayload = 512;

class RingSink : public mirror::Sink {
public:
    bool put(const std::uint8_t* data, std::size_t len, bool mayBlock) override {
        const std::size_t frames = (len + kMaxFramePayload - 1) / kMaxFramePayload;
        const std::size_t n = len + frames * mirror::kFrameHeader;
        if (ring_ == nullptr || n > kRingSize / 2) return false;
        if (ringFree() < n) {
            if (!mayBlock) return false;
            // Wait until the viewer has acknowledged enough, keeping USB
            // going meanwhile. Only between SPI transactions (mayBlock),
            // so nothing else on the bus is interrupted.
            const absolute_time_t giveUp = make_timeout_time_ms(kStallTimeoutMs);
            while (ringFree() < n) {
                if (!tud_cdc_n_connected(kItf) || time_reached(giveUp)) return false;
                if (msSince(lastHeard_) > kStallSilentMs) return false;   // nobody's reading
                serviceRx();
                pumpUsb();
                tud_task();
            }
        }
        for (std::size_t off = 0; off < len; off += kMaxFramePayload) {
            const std::size_t k = std::min(kMaxFramePayload, len - off);
            std::uint8_t hdr[mirror::kFrameHeader];
            mirror::frameHeader(hdr, sid_, wr_, data + off, k);
            ringWrite(hdr, sizeof(hdr));
            ringWrite(data + off, k);
        }
        return true;
    }
};

RingSink sink_;
mirror::Encoder encoder_(sink_);
DisplayDriver* display_ = nullptr;

enum class State { Off, Dump, Live };
State state_ = State::Off;
[[maybe_unused]] bool toldOff_ = false;      // "mirror is off" already logged for this viewer
[[maybe_unused]] bool viewerSeen_ = false;   // hipiview has asked on this connection

#ifdef DISPLAY_7INCH
mirror::Session session_(encoder_);
#endif

}  // namespace

// ── MirrorTransport ─────────────────────────────────────────────────────
MirrorTransport::MirrorTransport(RA8875Transport& inner) : inner_(inner) {}

void MirrorTransport::csLow() { encoder_.onCsLow(); inner_.csLow(); }
void MirrorTransport::csHigh() { inner_.csHigh(); encoder_.onCsHigh(); }
void MirrorTransport::rstLow() { encoder_.onReset(); inner_.rstLow(); }
void MirrorTransport::rstHigh() { inner_.rstHigh(); }
void MirrorTransport::spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) {
    inner_.spiTransfer(tx, rx, len);
    encoder_.onTransfer(tx, rx, len);
}

// ── Main loop side ──────────────────────────────────────────────────────
void displayMirror_init(DisplayDriver* display) { display_ = display; }

void displayMirror_addLayer(std::uint32_t addr, std::uint32_t rowBytes, std::uint32_t rowStep,
                            std::uint16_t rows, std::uint32_t assetId) {
#ifdef DISPLAY_7INCH
    mirror::Layer l;
    l.addr = addr; l.rowBytes = rowBytes; l.rowStep = rowStep; l.rows = rows; l.assetId = assetId;
    session_.addLayer(l);
#else
    (void)addr; (void)rowBytes; (void)rowStep; (void)rows; (void)assetId;
#endif
}

std::uint32_t displayMirror_fileAssetId(const char* path, std::uint32_t salt) {
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return 0;
    std::uint32_t h = mirror::fnv1a(path, std::strlen(path));
    const std::uint32_t meta[4] = { static_cast<std::uint32_t>(fno.fsize), fno.fdate, fno.ftime, salt };
    h = mirror::fnv1a(meta, sizeof(meta), h);
    return h != 0 ? h : 1;
}

bool displayMirror_active() { return state_ != State::Off; }

// Tells the viewer the setting (mirror_protocol.h, kMode)
static void sendMode() {
    if (state_ == State::Off || !encoder_.enabled()) return;
    const std::uint8_t p[2] = { mirror::kMode, static_cast<std::uint8_t>(mode_) };
    encoder_.emit(p, sizeof p);
}

void displayMirror_setMode(MirrorMode m) {
    if (m == mode_) return;
    mode_ = m;
    inputSaid_ = false;
    if (m != MirrorMode::Control) releaseInput();
    sendMode();
}
MirrorMode displayMirror_mode() { return mode_; }
const char* displayMirror_modeName(MirrorMode m) {
    switch (m) {
    case MirrorMode::View:    return "View";
    case MirrorMode::Control: return "Control";
    default:                  return "Off";
    }
}
void displayMirror_setInputHandlers(MirrorTouchHandler touch, MirrorButtonHandler button) {
    touchHandler_ = std::move(touch);
    buttonHandler_ = std::move(button);
}

void displayMirror_goodbye(std::uint32_t ms) {
    const absolute_time_t until = make_timeout_time_ms(ms);
#if defined(DISPLAY_7INCH) && HIPI_MIRROR_PORT && HIPI_MIRROR_TRANSPORT
    if (state_ != State::Off && encoder_.enabled()) {
        const std::uint8_t bye = mirror::kBye;
        encoder_.emit(&bye, 1);
    }
    while (!time_reached(until)) {
        tud_task();
        if (state_ == State::Off || ring_ == nullptr || !tud_cdc_n_connected(kItf)) continue;
        encoder_.flush();
        serviceRx();              // acknowledgements (input is only queued)
        pumpUsb();
    }
#else
    while (!time_reached(until)) tud_task();
#endif
}

static void pollMirror();

void displayMirror_poll() {
    pollMirror();
    deliverInput();          // here, at the top level -- see queueInput()
}

static void pollMirror() {
#if defined(DISPLAY_7INCH) && HIPI_MIRROR_PORT && HIPI_MIRROR_TRANSPORT
    if (display_ == nullptr) return;
    // Ends a running session: no more mirroring, the buffers go back
    auto endSession = [](const char* why) {
        if (state_ == State::Off) return;
        releaseInput();                      // nobody's finger on the panel any more
        encoder_.setEnabled(false);
        ringFree_();
        tud_cdc_n_write_clear(kItf);         // nothing stale for the next viewer
        state_ = State::Off;
        LOGF("\r\n * Display mirror: %s", why);
    };
    if (!tud_cdc_n_connected(kItf)) {
        endSession("viewer disconnected");
        requestMatch_ = 0;
        req_ = Req::Magic;
        requestPending_ = false;
        toldOff_ = false;
        viewerSeen_ = false;
        return;
    }
    serviceRx();
    // A (new) request from the viewer starts a session from scratch
    bool requested = requestPending_;
    requestPending_ = false;
    if (requested) viewerSeen_ = true;
    if (mode_ == MirrorMode::Off) {
        // Switched off in the settings: the port stays silent. Switching it
        // on again with hipiview still connected starts a session by itself
        // (below).
        endSession("switched off");
        if (requested && !toldOff_) {
            LOGF("\r\n * Display mirror: a viewer is asking, but the mirror is off (Settings > Screen)");
            toldOff_ = true;
        }
        return;
    }
    toldOff_ = false;
    // Switched on again while hipiview is still there: start right away
    // (with the picture ids from its last request)
    if (state_ == State::Off && viewerSeen_) requested = true;
    if (state_ == State::Off && !requested) return;     // port open, but not by hipiview
    // The viewer stopped reading (the ring stayed full, or nothing was
    // acknowledged for a long while): stop here and wait until it asks
    // again. Starting over by ourselves would only keep stalling HIPI
    // against a port nobody reads.
    if (state_ != State::Off && !requested) {
        if (wr_ == acked_) ringIdle_ = get_absolute_time();
        const bool gone = !encoder_.enabled() ||
            (msSince(ringIdle_) > kViewerGoneMs && msSince(lastHeard_) > kViewerGoneMs);
        if (gone) {
            endSession("the viewer stopped reading -- waiting until it asks again");
            viewerSeen_ = false;
            return;
        }
    }
    if (requested) {
        LOGF("\r\n * Display mirror: viewer connected, sending the screen");
        encoder_.setEnabled(false);
        tud_cdc_n_write_clear(kItf);
        ++sid_;
        if (!ringAlloc() || !encoder_.setEnabled(true)) {
            LOGF("\r\n * Display mirror: not enough memory");
            viewerSeen_ = false;             // don't retry until asked again
            encoder_.setEnabled(false);
            ringFree_();
            state_ = State::Off;
            return;
        }
        session_.setViewerAssets(ids_, idsWanted_);
        session_.start(*display_);
        inputSaid_ = false;
        sendMode();
        pumpUsb();
        state_ = State::Dump;
    }
    if (state_ == State::Dump && session_.step(*display_, kDumpBytesPerPoll)) {
        state_ = State::Live;
        LOGF("\r\n * Display mirror: in step");
    }
    encoder_.flush();
    // Nothing to send for a while: an empty frame, so both ends know the
    // other is still there
    if (state_ == State::Live && wr_ == acked_ && msSince(lastWrite_) > kHeartbeatMs) {
        std::uint8_t hdr[mirror::kFrameHeader];
        mirror::frameHeader(hdr, sid_, wr_, nullptr, 0);
        ringWrite(hdr, sizeof(hdr));
    }
    // Keep the USB port busy for a moment: one 64-byte packet goes per
    // transfer, and the next transfer only starts from tud_task()
    const absolute_time_t until = make_timeout_time_us(1500);
    do {
        serviceRx();
        pumpUsb();
        if (sent_ == wr_) break;
        tud_task();
    } while (!time_reached(until));
#endif
}

}  // namespace hipi
