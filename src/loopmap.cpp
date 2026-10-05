#define MODULE "LOOPMAP"
#include "loopmap.h"
#include "analyzer.h"
#include "plotterview.h"
#include "drive.h"
#include "pilbox.h"
#include "backlight.h"
#include "usb_serial.h"
#include "pico/time.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern std::vector<CDevice*> devices;   // hipi.cpp -- loop order

namespace hipi {

namespace {

// ── What is known about the loop ────────────────────────────────────────
enum class Src : std::uint8_t { Hipi, Pc, Before, After };

struct Node {
    bool used = false;
    Src src = Src::After;
    bool haveAid = false;
    std::uint8_t aid = 0;
    std::string id;                  // device ID (SDI answer)
    bool haveStatus = false;
    std::uint8_t status = 0;
    std::uint64_t lastT = 0;         // µs since start-up, 0 = never
    char lastWhat[24] = "";
    std::uint32_t frames = 0;
};
constexpr int kMaxAddr = 30;
Node nodes_[kMaxAddr + 1];           // by address 1..30

bool haveAad_ = false;
int hipiFirst_ = 0, hipiNext_ = 0;   // HIPI's range at the last auto-addressing [first, next)
std::uint64_t aadT_ = 0;
std::uint32_t cursor_ = 0;           // into the analyzer's capture
std::uint64_t lastFrameT_ = 0;
std::uint64_t roundTripUs_ = 0;      // time between two frames during a transfer

// Protocol state, to know who answers what
int talker_ = -1;
std::uint32_t listeners_ = 0;
IL_CMD_t pending_ = 0;               // SAI / SDI / SST / SDA waiting for its answer
std::string sdiBuf_;

bool dirty_ = true;
absolute_time_t nextDraw_ = nil_time;
DisplayDriver* d_ = nullptr;
int selected_ = 0;                   // address in the details box, 0 = none

CDevice* hipiDeviceAt(int addr) {
    for (CDevice* d : devices) {
        if (d->enabled() && static_cast<int>(d->addr()) == addr) return d;
    }
    return nullptr;
}

// What a HIPI device emulates (its card's ID line)
std::string modelOf(CDevice* d) {
    switch (d->type()) {
        case DISPLAY: return "HP82163";
        case DRIVE:   return "HP82161A";
        case PLOTTER: return "HP7470A";
        case PILBOX:  return "PIL-Box";
        default:      return "HIPI device";
    }
}

const char* aidClass(std::uint8_t aid) {
    switch (aid >> 4) {
        case 0x1: return "mass storage";
        case 0x2: return "printer";
        case 0x3: return "display";
        case 0x4: return "interface";
        case 0x5: return "instrument";
        case 0x6: return "graphics";
        default:  return "device";
    }
}

std::string mnem(IL_CMD_t f) {
    char b[32];
    ilMnemonic(f, b);
    std::string s(b);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void touch(int a, std::uint64_t t, const std::string& what) {
    if (a < 1 || a > kMaxAddr) return;
    Node& n = nodes_[a];
    if (!n.used) {
        // Not known from the auto-addressing: a device after HIPI
        n = Node();
        n.used = true;
        n.src = Src::After;
        dirty_ = true;
    }
    n.lastT = t;
    std::snprintf(n.lastWhat, sizeof(n.lastWhat), "%s", what.c_str());
}

void onAutoAddress(int first, int next, std::uint64_t t) {
    haveAad_ = true;
    aadT_ = t;
    hipiFirst_ = first;
    hipiNext_ = next;
    for (int a = 1; a <= kMaxAddr; ++a) {
        Node& n = nodes_[a];
        Src src;
        bool known = true;
        if (a < first)      src = Src::Before;
        // (In CON the PC is the controller, not a place for devices: the
        // other addresses are devices on the cable)
        else if (a < next)  src = hipiDeviceAt(a) ? Src::Hipi
                                : (pilbox != nullptr && pilbox->mode() == CON) ? Src::After : Src::Pc;
        else                known = false;
        if (!known) {
            // After HIPI: not part of this auto-addressing's view -- a device
            // already seen there keeps what's known (the HP-41 re-addresses
            // often, usually to the same devices); new ones show up when used
            if (n.used && n.src != Src::After) n = Node();
            continue;
        }
        if (!n.used || n.src != src) {      // another device at this address now
            n = Node();
            n.used = true;
            n.src = src;
        }
        if (src == Src::Hipi) {             // HIPI's own: known without asking
            CDevice* d = hipiDeviceAt(a);
            n.haveAid = true;
            n.aid = static_cast<std::uint8_t>(d->accessoryId());
            if (n.id.empty()) n.id = modelOf(d);
        }
    }
    dirty_ = true;
    LOGF("\r\n * Loop map: auto-address -- %d before HIPI, HIPI %d..%d", first - 1, first, next - 1);
}

void onFrame(IL_CMD_t in, IL_CMD_t out, std::uint64_t t) {
    const bool absorbed = out == IL_NO_FRAME;
    if (lastFrameT_ != 0 && pending_ != 0) roundTripUs_ = t - lastFrameT_;
    lastFrameT_ = t;

    // The talker's last byte came back to it (on HIPI's side of the loop:
    // HIPI's own talker, or a PC device behind PILBOX) and it ended the
    // transfer: the frame leaves as ETO/ETE, not as another byte
    if (IS_DATA(in) && !absorbed && in != out && !IS_DATA(out)) {
        onFrame(out, out, t);
        return;
    }
    if (IS_DATA(in)) {
        const int byte = (!absorbed && in != out && IS_DATA(out)) ? (out & 0xFF) : (in & 0xFF);
        if (talker_ >= 1 && talker_ <= kMaxAddr && pending_ != 0) {
            Node& n = nodes_[talker_];
            ++n.frames;
            if (pending_ == SAI) {
                n.haveAid = true;
                n.aid = static_cast<std::uint8_t>(byte);
                pending_ = 0;
                dirty_ = true;
            } else if (pending_ == SST) {
                n.haveStatus = true;
                n.status = static_cast<std::uint8_t>(byte);
                pending_ = 0;
                dirty_ = true;
            } else if (pending_ == SDI) {
                if (byte >= 32 && byte < 127 && sdiBuf_.size() < 20) sdiBuf_ += static_cast<char>(byte);
            }
        } else {
            for (int a = 1; a <= kMaxAddr; ++a)
                if (listeners_ & (1u << a)) ++nodes_[a].frames;
        }
        return;
    }
    if (in >= AAD && in < AAD + 32) {
        const int first = in & 0x1F;
        const int next = (!absorbed && out >= AAD && out < AAD + 32) ? (out & 0x1F) : first;
        onAutoAddress(first, next, t);
        return;
    }
    const int a = in & 0x1F;
    if (in >= LAD && in < UNL)       { listeners_ |= 1u << a; touch(a, t, "listener"); }
    else if (in == UNL)              { listeners_ = 0; }
    else if (in >= TAD && in < UNT)  { talker_ = a; touch(a, t, "talker"); }
    else if (in == UNT)              { talker_ = -1; }
    else if (in == IFC)              { talker_ = -1; listeners_ = 0; pending_ = 0; }
    else if (in == SAI || in == SDI || in == SST || in == SDA) {
        pending_ = in;
        sdiBuf_.clear();
        if (talker_ >= 1) touch(talker_, t, mnem(in));
        // HIPI's own talker answered in this same frame
        if (!absorbed && in != out && IS_DATA(out)) onFrame(out, out, t);
    } else if (in == ETO || in == ETE || in == NRD || (!absorbed && (out == ETO || out == ETE))) {
        // (HIPI's own answer SDI with their device name -- they keep the
        // model they emulate as their ID line instead)
        if (pending_ == SDI && talker_ >= 1 && talker_ <= kMaxAddr && !sdiBuf_.empty() &&
            nodes_[talker_].src != Src::Hipi) {
            nodes_[talker_].id = sdiBuf_;
            dirty_ = true;
        }
        pending_ = 0;
    } else if (in >= DDL && in < DDL + 32) {
        for (int x = 1; x <= kMaxAddr; ++x)
            if (listeners_ & (1u << x)) touch(x, t, mnem(in));
    } else if (in >= DDT && in < DDT + 32) {
        if (talker_ >= 1) touch(talker_, t, mnem(in));
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────
constexpr std::uint16_t kYellow = 0xEDC0, kBlack = 0x0000, kWhite = 0xFFFF;
constexpr std::uint16_t kGrey = 0x7BEF, kDark = 0x39C7, kCard = 0x10A2;
constexpr std::uint16_t kBlue = 0x563C, kOrange = 0xF4A5;

std::uint16_t srcColor(Src s) {
    switch (s) {
        case Src::Hipi: return kYellow;
        case Src::Pc:   return kBlue;
        default:        return kOrange;
    }
}
const char* srcText(Src s) {
    switch (s) {
        case Src::Hipi:   return "HIPI";
        case Src::Pc:     return "PC via PILBOX";
        case Src::Before: return "loop, before HIPI";
        default:          return "loop, after HIPI";
    }
}

struct Card { int addr; int x, y, w, h; };
std::vector<Card> cards_;
int ctrlX_ = 0, ctrlY_ = 0;
enum class CardMode { Full, Compact, Tile } mode_ = CardMode::Full;

// Devices in loop order: before HIPI, HIPI's range, then the ones after
std::vector<int> loopOrder() {
    std::vector<int> v;
    for (int a = 1; a <= kMaxAddr; ++a)
        if (nodes_[a].used) v.push_back(a);
    return v;
}

void text(int x, int y, const char* s, std::uint8_t scale, std::uint16_t fg, std::uint16_t bg) {
    d_->txtSize(scale);
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s);
}

std::string timeText(std::uint64_t t) {
    if (t == 0) return "--";
    const std::uint64_t ms = t / 1000;
    char b[16];
    std::snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", static_cast<unsigned long>(ms / 3600000),
                  static_cast<unsigned long>((ms / 60000) % 60), static_cast<unsigned long>((ms / 1000) % 60));
    return b;
}

void layout(const std::vector<int>& order) {
    const int n = static_cast<int>(order.size());
    int cw, ch;
    if (n <= 8)       { mode_ = CardMode::Full;    cw = 186; ch = 88; }
    else if (n <= 16) { mode_ = CardMode::Compact; cw = 150; ch = 52; }
    else              { mode_ = CardMode::Tile;    cw = 72;  ch = 44; }
    const int L = 150, T = 50 + ch / 2, R = SCREEN_MAX_X - 20 - cw / 2, B = SCREEN_MAX_Y - 44 - ch / 2;
    const int top = R - L, side = B - T, per = top + side + top;
    cards_.clear();
    for (int i = 0; i < n; ++i) {
        const int s = static_cast<int>((i + 0.5) * per / n);
        int x, y;
        if (s < top)              { x = L + s;                y = T; }
        else if (s < top + side)  { x = R;                    y = T + (s - top); }
        else                      { x = R - (s - top - side); y = B; }
        cards_.push_back(Card{ order[static_cast<std::size_t>(i)], x - cw / 2, y - ch / 2, cw, ch });
    }
    ctrlX_ = 14;
    ctrlY_ = (T + B) / 2 - 50;
    // The ring itself
    d_->roundRect(80, T, R - 80, B - T, 30, kYellow);
    d_->roundRect(81, T + 1, R - 82, B - T - 2, 29, kYellow);
}

// A card's first line: HIPI's device name, else the device ID once the
// HP-41 has asked for it (the address is on the card anyway), else "addr n"
std::string cardName(int a) {
    if (CDevice* d = hipiDeviceAt(a)) return d->name();
    if (!nodes_[a].id.empty()) return nodes_[a].id;
    char b[12];
    std::snprintf(b, sizeof(b), "addr %d", a);
    return b;
}

// A card's second line: what HIPI's device emulates, else the type from
// the accessory ID ("graphics", "mass storage"...), else "?"
std::string cardType(int a) {
    const Node& n = nodes_[a];
    if (n.src == Src::Hipi && !n.id.empty()) return n.id;
    if (n.haveAid) return aidClass(n.aid);
    return "?";
}

void drawCard(const Card& c) {
    const Node& n = nodes_[c.addr];
    const std::uint16_t col = srcColor(n.src);
    char b[40];
    std::snprintf(b, sizeof(b), "%2d", c.addr);
    if (mode_ == CardMode::Tile) {
        d_->fillRoundRect(c.x, c.y, c.w, c.h, 8, col);
        text(c.x + 8, c.y + 2, b, 1, kBlack, col);
        const std::string nm = cardName(c.addr).substr(0, 8);
        text(c.x + 4, c.y + 28, nm.c_str(), 0, kBlack, col);
        return;
    }
    d_->fillRoundRect(c.x, c.y, c.w, c.h, 8, kCard);
    d_->roundRect(c.x, c.y, c.w, c.h, 8, col);
    d_->fillRoundRect(c.x, c.y, 36, c.h, 8, col);
    d_->fillRect(c.x + 26, c.y, 10, c.h, col);
    text(c.x + 2, c.y + c.h / 2 - 16, b, 1, kBlack, col);
    text(c.x + 42, c.y + 6, cardName(c.addr).c_str(), 0, kWhite, kCard);
    text(c.x + 42, c.y + 26, cardType(c.addr).c_str(), 0, kYellow, kCard);
    if (mode_ == CardMode::Full) {
        if (n.haveAid) std::snprintf(b, sizeof(b), "accessory ID 0x%02X", n.aid);
        else           std::snprintf(b, sizeof(b), "accessory ID ?");
        text(c.x + 42, c.y + 46, b, 0, kGrey, kCard);
        text(c.x + 42, c.y + 66, srcText(n.src), 0, col, kCard);
    }
}

void drawDetails() {
    if (selected_ < 1 || selected_ > kMaxAddr || !nodes_[selected_].used) { selected_ = 0; return; }
    const Node& n = nodes_[selected_];
    const int w = 480, h = 236, x = (SCREEN_MAX_X - w) / 2, y = (SCREEN_MAX_Y - h) / 2;
    d_->fillRect(x, y, w, h, kBlack);
    d_->roundRect(x, y, w, h, 12, kYellow);
    d_->roundRect(x + 1, y + 1, w - 2, h - 2, 11, kYellow);
    char title[40];
    std::snprintf(title, sizeof(title), " %s (%d) ", cardName(selected_).c_str(), selected_);
    d_->fillRect(x + 16, y - 9, static_cast<int>(std::strlen(title)) * 8 + 8, 18, kYellow);
    text(x + 20, y - 8, title, 0, kBlack, kYellow);
    char v[64];
    struct Row { const char* k; std::string v; } rows[7];
    int r = 0;
    rows[r++] = { "Where", srcText(n.src) };
    std::snprintf(v, sizeof(v), "%d", selected_);
    rows[r++] = { "Address", v };
    if (n.src == Src::Hipi) {
        rows[r++] = { "Emulates", n.id };
    } else {
        rows[r++] = { "Device ID", n.id.empty() ? "? (not asked yet)" : n.id };
    }
    if (n.haveAid) std::snprintf(v, sizeof(v), "0x%02X  %s", n.aid, aidClass(n.aid));
    else           std::snprintf(v, sizeof(v), "? (not asked yet)");
    rows[r++] = { "Accessory ID", v };
    // Status: HIPI's drive tells its cassette; others the last SST answer
    std::string status = n.haveStatus ? std::to_string(n.status) : std::string("?");
    if (CDevice* d = hipiDeviceAt(selected_)) {
        if (d->type() == DRIVE) {
            const std::string& f = static_cast<CDrive*>(d)->mediaFile();
            status = f.empty() ? "no cassette" : "cassette " + f;
        } else if (!n.haveStatus) {
            status = "ready";
        }
    }
    rows[r++] = { "Status", status };
    rows[r++] = { "Last used", n.lastT ? timeText(n.lastT) + "  " + n.lastWhat : std::string("--") };
    std::snprintf(v, sizeof(v), "%lu data frames", static_cast<unsigned long>(n.frames));
    rows[r++] = { "Traffic", v };
    for (int i = 0; i < r; ++i) {
        text(x + 20, y + 22 + i * 28, rows[i].k, 0, kGrey, kBlack);
        text(x + 150, y + 22 + i * 28, rows[i].v.c_str(), 0, kWhite, kBlack);
    }
    text(x + w - 150, y + h - 22, "tap to close", 0, kDark, kBlack);
}

bool visible() {
    return d_ != nullptr && plotterview_output() == DisplayOutput::LoopMap && !plotterview_isSplashVisible();
}

// What the header line says (padded, so a shorter text overwrites a longer one)
std::string headerText(int devices) {
    char b[96];
    if (haveAad_) {
        std::snprintf(b, sizeof(b), "last auto-address %s   %d devices   frame round trip %lu.%lu ms",
                      timeText(aadT_).c_str(), devices,
                      static_cast<unsigned long>(roundTripUs_ / 1000), static_cast<unsigned long>((roundTripUs_ / 100) % 10));
    } else {
        std::snprintf(b, sizeof(b), "waiting for the controller to set up the loop (auto-address)");
    }
    std::string s(b);
    s.resize(96, ' ');
    return s;
}

// What a card shows -- to redraw only the cards that changed (5")
std::string cardSignature(int a) {
    const Node& n = nodes_[a];
    char b[32];
    std::snprintf(b, sizeof(b), "|%d|%d|%02X|", static_cast<int>(n.src), n.haveAid ? 1 : 0, n.aid);
    return cardName(a) + b + n.id;
}

// Last drawn state, see loopmap_poll()
std::vector<int> drawnOrder_;
std::vector<std::string> drawnSigs_;
int drawnSelected_ = 0;
std::string drawnHeader_;
std::string drawnDetails_;

std::string detailsSignature() {
    if (!selected_) return "";
    const Node& n = nodes_[selected_];
    return cardSignature(selected_) + "|" + std::to_string(n.lastT) + n.lastWhat + "|" +
           std::to_string(n.frames) + "|" + std::to_string(n.haveStatus ? n.status : -1);
}

void rememberDrawn(const std::vector<int>& order) {
    drawnOrder_ = order;
    drawnSigs_.clear();
    for (int a : order) drawnSigs_.push_back(cardSignature(a));
    drawnSelected_ = selected_;
    drawnHeader_ = headerText(static_cast<int>(order.size()));
    drawnDetails_ = detailsSignature();
}

// The whole map, on the current canvas
void renderFull() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, kBlack);
    // Title bar
    d_->fillRect(0, 0, SCREEN_MAX_X, 26, kYellow);
    text(8, 5, "HP-IL LOOP MAP", 0, kBlack, kYellow);
    const std::vector<int> order = loopOrder();
    text(200, 5, headerText(static_cast<int>(order.size())).c_str(), 0, kBlack, kYellow);
    // Controller, ring, devices
    if (!order.empty()) {
        layout(order);
        for (const Card& c : cards_) drawCard(c);
    } else {
        cards_.clear();
        ctrlY_ = SCREEN_MAX_Y / 2 - 50;
    }
    d_->fillRoundRect(ctrlX_, ctrlY_, 116, 100, 10, kDark);
    d_->roundRect(ctrlX_, ctrlY_, 116, 100, 10, kWhite);
    text(ctrlX_ + 22, ctrlY_ + 14, "CTRL", 1, kWhite, kDark);
    text(ctrlX_ + 18, ctrlY_ + 56, "controller", 0, kGrey, kDark);
    // Legend, switched-off HIPI devices
    int x = 20;
    text(x, SCREEN_MAX_Y - 26, "Off:", 0, kGrey, kBlack);
    x += 40;
    for (CDevice* dv : devices) {
        if (dv->enabled() || x > 420) continue;
        text(x, SCREEN_MAX_Y - 26, dv->name(), 0, kDark, kBlack);
        x += static_cast<int>(std::strlen(dv->name())) * 8 + 16;
    }
    struct { std::uint16_t c; const char* t; } leg[] = { { kYellow, "HIPI" }, { kBlue, "PC" }, { kOrange, "other" } };
    x = 560;
    for (auto& l : leg) {
        d_->fillRect(x, SCREEN_MAX_Y - 24, 12, 12, l.c);
        text(x + 18, SCREEN_MAX_Y - 26, l.t, 0, kWhite, kBlack);
        x += 18 + static_cast<int>(std::strlen(l.t)) * 8 + 24;
    }
    text(SCREEN_MAX_X - 230, SCREEN_MAX_Y - 26, "tap a device for details", 0, kGrey, kBlack);
    if (selected_) drawDetails();
    rememberDrawn(order);
}

}  // namespace

void loopmap_drawAll() {
    if (d_ == nullptr) return;
    dirty_ = false;
#ifdef DISPLAY_7INCH
    // Flicker-free: the map is drawn in an off-screen layer, then copied
    // to the panel in one go inside the display chip (BTE)
    const std::uint32_t prevAddr = d_->canvasAddr();
    const std::uint16_t prevStride = d_->canvasStride();
    d_->beginLayerDraw(LT7683::kBteLayer3Addr);
    renderFull();
    if (prevAddr == 0) d_->endOverlayDraw();
    else               d_->beginLayerDraw(prevAddr, prevStride);
    d_->copyLayerToPanel(LT7683::kBteLayer3Addr, 0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
#else
    renderFull();
#endif
    if (!loopOrder().empty()) backlight_activity();  // (only when something changed)
}

#ifndef DISPLAY_7INCH
namespace {

// 5" (no off-screen layers): redraw only what changed -- the header text
// (padded, no clearing), the cards whose content changed, the details box
void updateChanged() {
    const std::vector<int> order = loopOrder();
    if (order != drawnOrder_ || selected_ != drawnSelected_ || order.empty()) {
        loopmap_drawAll();                   // the layout changed: everything
        return;
    }
    dirty_ = false;
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    const std::string header = headerText(static_cast<int>(order.size()));
    if (header != drawnHeader_) text(200, 5, header.c_str(), 0, kBlack, kYellow);
    bool any = false;
    for (std::size_t i = 0; i < cards_.size() && i < order.size(); ++i) {
        const std::string sig = cardSignature(order[i]);
        if (i < drawnSigs_.size() && sig == drawnSigs_[i]) continue;
        if (selected_ == 0) drawCard(cards_[i]);   // (under the details box otherwise)
        any = true;
    }
    if (selected_ && detailsSignature() != drawnDetails_) drawDetails();
    rememberDrawn(order);
    if (any) backlight_activity();
}

}  // namespace
#endif

void loopmap_init(DisplayDriver* display) {
    d_ = display;
}

void loopmap_poll() {
    IL_CMD_t in, out;
    std::uint64_t t;
    for (int n = 0; n < 64 && analyzer_nextFrame(cursor_, in, out, t); ++n) onFrame(in, out, t);
    if (dirty_ && visible() && time_reached(nextDraw_)) {
        nextDraw_ = make_timeout_time_ms(500);
#ifdef DISPLAY_7INCH
        loopmap_drawAll();                   // off-screen + copy: no flicker
#else
        updateChanged();                     // only what changed
#endif
    }
}

bool loopmap_tap(int x, int y) {
    if (selected_) {                                 // the details box is open: any tap closes it
        selected_ = 0;
        loopmap_drawAll();
        return true;
    }
    for (const Card& c : cards_) {
        if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) {
            selected_ = c.addr;
            loopmap_drawAll();
            return true;
        }
    }
    return false;
}

}  // namespace hipi
