#define MODULE "PRINTER"

#include "printer.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include "printer_font.h"
#include "usb_serial.h"
#include <cctype>
#include <cstdlib>

extern bool bTrace;

namespace {
// A key press nobody has asked about is forgotten after this long
// (the HP-41 off, or not polling): it shouldn't fire much later.
constexpr std::uint32_t kKeyLatchMs = 3000;
}

const char* CPrinter::modeName(PrinterMode m) {
    switch (m) {
        case PrinterMode::Trace: return "TRACE";
        case PrinterMode::Norm:  return "NORM";
        default:                 return "MAN";
    }
}

void CPrinter::reset() {
    eightBit_ = false;
    modes_ = 0;
    inEscape_ = false;
    escBuf_.clear();
    graphicsLeft_ = 0;
    barcodeLeft_ = 0;
    building_ = Line();
    used_ = 0;
    statusLeft_ = 0;
    srq_ = false;
    printLatched_ = advanceLatched_ = false;
}

void CPrinter::clear() {
    MTRC_LOGF("clear (DCL/SDC)");
    reset();
}

std::uint16_t CPrinter::status() const {
    std::uint16_t st = 0;
    if (used_ == 0) st |= kEL | kBE;
    st |= kID;                                   // never busy: printing is instant
    if (mode_ == PrinterMode::Norm)  st |= kMB;
    if (mode_ == PrinterMode::Trace) st |= kMA;
    if (printDown_ || (printLatched_ && !time_reached(printLatchEnd_))) st |= kPR;
    if (advanceDown_ || (advanceLatched_ && !time_reached(advanceLatchEnd_))) st |= kPA;
    if (srq_) st |= kSR;
    if (eightBit_) st |= kEB;
    if (modes_ & kModeRjust)  st |= kRJ;
    if (modes_ & kModeDouble) st |= kDW;
    if (modes_ & kModeColumn) st |= kCO;
    if (modes_ & kModeLower)  st |= kLC;
    return st;
}

// ── Front panel keys ────────────────────────────────────────────────────
// Pressing sets SR (service request) and PR / PA. The bit stays in the
// status while the key is held, and -- for a quick tap -- until the
// controller has read it once (or kKeyLatchMs has passed).
void CPrinter::pressPrint(bool down) {
    if (down && !printDown_) {
        printLatched_ = true;
        printLatchEnd_ = make_timeout_time_ms(kKeyLatchMs);
        srq_ = true;
        MTRC_LOGF("PRINT pressed");
    }
    printDown_ = down;
}

void CPrinter::pressAdvance(bool down) {
    if (down && !advanceDown_) {
        // The paper moves whether or not the controller is listening
        feedPaper();
        if (!(modes_ & kModeInhibitAdv)) {
            advanceLatched_ = true;
            advanceLatchEnd_ = make_timeout_time_ms(kKeyLatchMs);
            srq_ = true;
        }
        MTRC_LOGF("PAPER ADVANCE pressed");
    }
    advanceDown_ = down;
}

// ── HP-IL ───────────────────────────────────────────────────────────────
IL_CMD_t CPrinter::hpil(IL_CMD_t cmd) {
    ownResponse_ = false;
    // A request nobody came for (see kKeyLatchMs) is withdrawn
    if (srq_ && !printDown_ && !advanceDown_ &&
        time_reached(printLatchEnd_) && time_reached(advanceLatchEnd_))
        srq_ = false;
    const bool ident = isTalker() && (cmd == SAI || cmd == SDI);
    IL_CMD_t rtn = CDevice::hpil(cmd);
    // Service request: on every data and IDY frame passing by -- but not
    // on our own answers (the status bytes, SAI/SDI)
    if (srq_ && !ownResponse_ && !ident && rtn != IL_NO_FRAME && (IS_DATA(rtn) || IS_IDLE(rtn)))
        rtn = static_cast<IL_CMD_t>(rtn | SRQ_BIT);
    return rtn;
}

void CPrinter::doListener(IL_CMD_t cmd, IL_CMD_t* rtn) {
    *rtn = cmd;
    if (IS_DATA(cmd)) feed(static_cast<std::uint8_t>(cmd & 0xFF));
}

// SST: the two status bytes (low first), each sent when the previous one
// comes back round the loop, then ETO. SDA: nothing to send (ETO).
void CPrinter::doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) {
    if (cmd == NRD) {                         // no more wanted: ETO next
        if (statusLeft_ > 1) statusLeft_ = 1;
        return;
    }
    if (cmd == SST) {
        statusSent_ = static_cast<std::uint16_t>(status() & ~kSR);
        // Read: the request is answered, a tap has been seen
        srq_ = false;
        printLatched_ = advanceLatched_ = false;
        *rtn = static_cast<IL_CMD_t>(statusSent_ & 0xFF);
        statusLeft_ = 2;
        ownResponse_ = true;
        MTRC_LOGF("SST -> %02X %02X", statusSent_ & 0xFF, statusSent_ >> 8);
        return;
    }
    if (cmd == SDA) {
        *rtn = ETO;
        return;
    }
    if (IS_DATA(cmd) && statusLeft_ > 0) {    // our byte, back round the loop
        if (statusLeft_ == 2) {
            *rtn = static_cast<IL_CMD_t>(statusSent_ >> 8);
            ownResponse_ = true;
        } else {
            *rtn = ETO;
        }
        --statusLeft_;
    }
}

void CPrinter::show() {
    CDevice::show();
    LOGF(" mode:%s int:%u 8bit:%d modes:%02X line:%u", modeName(mode_),
         static_cast<unsigned>(intensity_), eightBit_ ? 1 : 0, modes_,
         static_cast<unsigned>(used_));
}

// ── Characters ──────────────────────────────────────────────────────────
// One byte from the controller. Order as in the real printer (and
// pyILPER): pending graphics / bar code bytes first, then an escape
// sequence in progress, CR / LF, the 8-bit mode's codes (0x80..0xFF,
// ignored outside 8-bit mode), and the rest is a character -- or a
// graphics column in column mode.
void CPrinter::feed(std::uint8_t c) {
    if (graphicsLeft_ > 0) { --graphicsLeft_; addColumn(c); return; }
    if (barcodeLeft_ > 0)  { --barcodeLeft_;  return; }     // (not drawn)
    if (inEscape_) {
        if (escBuf_.empty() && c == '|') {    // ESC | : 8-bit mode
            inEscape_ = false;
            eightBit_ = true;
            return;
        }
        escBuf_ += static_cast<char>(c);
        if (c >= 'A' && c <= 'Z') {           // an upper case letter ends it
            inEscape_ = false;
            escape(escBuf_);
        } else if (escBuf_.size() > 16) {     // not a sequence after all
            inEscape_ = false;
        }
        return;
    }
    if (c == 27 && !eightBit_) {
        inEscape_ = true;
        escBuf_.clear();
        return;
    }
    const bool column = (modes_ & kModeColumn) != 0;
    if (c == 0x0D && !column) { endOfLine(); return; }
    if (c == 0x0A && !column) return;          // LF: ignored
    if (c >= 0x80) {
        if (!eightBit_) return;
        if (c <= 0x8F) {                      // bar code: n bytes follow
            barcodeLeft_ = c & 0x0F;
        } else if (c >= 0xA0 && c <= 0xB7) {  // skip n characters
            addBlank((c - 0xA0) * 7);
        } else if (c >= 0xB8 && c <= 0xBF) {  // skip n columns
            addBlank(c - 0xB8);
        } else if (c >= 0xD0 && c <= 0xDF) {  // lower case, column, double wide
            modes_ = static_cast<std::uint8_t>((modes_ & ~0x07) | (c & 0x07));
        } else if (c == 0xE0) {
            modes_ &= static_cast<std::uint8_t>(~kModeRjust);
        } else if (c == 0xE8) {
            modes_ |= kModeRjust;
        } else if (c == 0xFC) {
            eightBit_ = false;
        } else if (c == 0xFE) {
            modes_ &= static_cast<std::uint8_t>(~kModeInhibitAdv);
        } else if (c == 0xFF) {
            modes_ |= kModeInhibitAdv;
        }
        // 0xC0 (format specifier) and the rest: not used
        return;
    }
    if (column) addColumn(c);
    else        addChar(c);
}

// The escape sequences (the rest is stepped over):
//   &k0S / &k1S   single / double wide       &l0J / &l1J  left / right
//   *b<n>G        n graphics columns follow  *z<n>B       n bar code bytes
//   &a+<n>C       skip n characters          &a+<n>D      skip n columns
void CPrinter::escape(const std::string& seq) {
    MTRC_LOGF("ESC %s", seq.c_str());
    if (seq == "&k0S") { modes_ &= static_cast<std::uint8_t>(~kModeDouble); return; }
    if (seq == "&k1S") { modes_ |= kModeDouble; return; }
    if (seq == "&l0J") { modes_ &= static_cast<std::uint8_t>(~kModeRjust); return; }
    if (seq == "&l1J") { modes_ |= kModeRjust; return; }
    const char last = seq.empty() ? 0 : seq.back();
    if (seq.rfind("*b", 0) == 0 && last == 'G') {
        graphicsLeft_ = std::atoi(seq.c_str() + 2);
        return;
    }
    if (seq.rfind("*z", 0) == 0 && last == 'B') {
        barcodeLeft_ = std::atoi(seq.c_str() + 2);
        return;
    }
    if (seq.rfind("&a+", 0) == 0 && (last == 'C' || last == 'D')) {
        const int n = std::atoi(seq.c_str() + 3);
        if (n > 0 && n <= kDots) addBlank(last == 'C' ? n * 7 : n);
    }
}

// Room for `columns` more on the line? If not, the line is printed first
// (the real printer does the same), so the new part starts a new line.
bool CPrinter::room(int columns) {
    if (used_ + columns <= kDots) return true;
    endOfLine();
    return columns <= kDots;
}

void CPrinter::addChar(std::uint8_t c) {
    if ((modes_ & kModeLower) && c >= 'A' && c <= 'Z') c = static_cast<std::uint8_t>(c - 'A' + 'a');
    const bool dw = (modes_ & kModeDouble) != 0;
    if (!room(dw ? 14 : 7)) return;
    const std::uint8_t (*font)[5] = eightBit_ ? kPrinterFontHp41 : kPrinterFontAscii;
    const int step = dw ? 2 : 1;
    used_ += step;                                     // blank column before
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < step; ++k) building_.dots[static_cast<std::size_t>(used_++)] = font[c & 0x7F][i];
    used_ += step;                                     // and after
    // The text, for the log: HP-41 characters below 32 shown as '.'
    building_.text += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
}

void CPrinter::addColumn(std::uint8_t bits) {
    const bool dw = (modes_ & kModeDouble) != 0;
    if (!room(dw ? 2 : 1)) return;
    building_.dots[static_cast<std::size_t>(used_++)] = bits;
    if (dw) building_.dots[static_cast<std::size_t>(used_++)] = bits;
}

void CPrinter::addBlank(int columns) {
    if (modes_ & kModeDouble) columns *= 2;
    if (columns <= 0 || !room(columns)) return;
    used_ += columns;
    building_.text.append(static_cast<std::size_t>((columns + 6) / 7), ' ');
}

void CPrinter::endOfLine() {
    Line done = std::move(building_);
    if ((modes_ & kModeRjust) && used_ < kDots) {     // right justified
        const int shift = kDots - used_;
        for (int i = used_ - 1; i >= 0; --i) {
            done.dots[static_cast<std::size_t>(i + shift)] = done.dots[static_cast<std::size_t>(i)];
            done.dots[static_cast<std::size_t>(i)] = 0;
        }
        done.text.insert(0, static_cast<std::size_t>(shift / 7), ' ');
    }
    building_ = Line();
    used_ = 0;
    addLine(std::move(done));
}

void CPrinter::feedPaper() {
    addLine(Line());
}

// ── The paper ─────────────────────────────────────────────────────────

namespace {
// Without a store of the firmware's: kMaxLines records in RAM
class RamPaperStore : public PaperStore {
public:
    std::uint32_t capacity() const override { return CPrinter::kMaxLines; }
    void write(std::uint32_t record, const std::uint8_t* data) override {
        std::memcpy(&mem_[record * CPrinter::kRecordBytes], data, CPrinter::kRecordBytes);
    }
    void read(std::uint32_t record, std::uint8_t* data) override {
        std::memcpy(data, &mem_[record * CPrinter::kRecordBytes], CPrinter::kRecordBytes);
    }
private:
    std::vector<std::uint8_t> mem_ = std::vector<std::uint8_t>(CPrinter::kMaxLines * CPrinter::kRecordBytes);
};
}  // namespace

PaperStore& CPrinter::store() const {
    if (store_ != nullptr) return *store_;
    if (!ramStore_) ramStore_.reset(new RamPaperStore());
    return *ramStore_;
}

void CPrinter::setPaperStore(PaperStore* store) {
    store_ = store;
    firstKept_ = lineCount_;                 // (the paper starts empty)
    cacheValid_ = false;
}

std::uint32_t CPrinter::paperCapacity() const { return store().capacity(); }

std::uint32_t CPrinter::firstKept() const {
    const std::uint32_t cap = paperCapacity();
    const std::uint32_t oldest = lineCount_ > cap ? lineCount_ - cap : 0;
    return std::max(firstKept_, oldest);
}

void CPrinter::storeLine(std::uint32_t n, const Line& l) {
    std::uint8_t rec[kRecordBytes] = {};
    std::memcpy(rec, l.dots.data(), kDots);
    const std::size_t len = std::min(l.text.size(), kMaxText);
    rec[kDots] = static_cast<std::uint8_t>(len);
    std::memcpy(rec + kDots + 1, l.text.data(), len);
    PaperStore& s = store();
    s.write(n % s.capacity(), rec);
    if (cacheValid_ && cacheN_ == n) cacheValid_ = false;
}

bool CPrinter::readLine(std::uint32_t n, Line& out) const {
    if (n < firstKept() || n >= lineCount_) return false;
    if (cacheValid_ && cacheN_ == n) { out = cache_; return true; }
    std::uint8_t rec[kRecordBytes];
    PaperStore& s = store();
    s.read(n % s.capacity(), rec);
    std::memcpy(out.dots.data(), rec, kDots);
    const std::size_t len = std::min<std::size_t>(rec[kDots], kMaxText);
    out.text.assign(reinterpret_cast<const char*>(rec + kDots + 1), len);
    return true;
}

const CPrinter::Line* CPrinter::line(std::uint32_t n) const {
    if (cacheValid_ && cacheN_ == n && n >= firstKept() && n < lineCount_) return &cache_;
    if (!readLine(n, cache_)) return nullptr;
    cacheN_ = n;
    cacheValid_ = true;
    return &cache_;
}

void CPrinter::setLine(std::uint32_t n, const Line& l) {
    if (n < firstKept() || n >= lineCount_) return;
    storeLine(n, l);
}

std::uint32_t CPrinter::startPaper(std::uint32_t n) {
    // Blank lines first (a record left from before isn't shown as one of them)
    const std::uint32_t first = lineCount_;
    lineCount_ += n;
    firstKept_ = first;
    cacheValid_ = false;
    const Line blank;
    for (std::uint32_t k = firstKept(); k < lineCount_; ++k) storeLine(k, blank);
    return first;
}

void CPrinter::addLine(Line&& line) {
    if (bTrace) MLOGF("|%s|", line.text.c_str());
    storeLine(lineCount_, line);
    ++lineCount_;
    // (the callback gets the line as it was kept: the text perhaps shortened)
    if (line.text.size() > kMaxText) line.text.resize(kMaxText);
    if (onLine_) onLine_(line);
}
