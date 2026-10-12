#ifndef __PRINTER_H__
#define __PRINTER_H__

// CPrinter -- an HP 82162A thermal printer on the HP-IL loop (TFPRINT),
// with its own view on the panel: a picture of the printer with working
// controls (plotterview.cpp, Printer view).
//
// The HP-IL side follows the real printer and pyILPER's emulation of it
// (pilhp82162a.py, itself derived from Eric Smith's nonpareil):
//   accessory ID 0x20 (printer), address 1 after AAU, no device ID,
//   two status bytes (SST, least significant byte first):
//     first byte  (low)   SR 0x40  service request (PRINT/ADV pressed)
//                         MB 0x20  mode switch NORM
//                         MA 0x10  mode switch TRACE   (MAN: neither)
//                         ER 0x08  error
//                         PA 0x04  PAPER ADVANCE pressed
//                         PR 0x02  PRINT pressed
//                         LA 0x01  low battery         (never)
//     second byte (high)  EL 0x80  end of line         ID 0x40 idle
//                         BE 0x20  buffer empty        EB 0x10 eight-bit mode
//                         RJ 0x08  right justify       DW 0x04 double wide
//                         CO 0x02  column mode         LC 0x01 lower case
//   While SR is set, the printer adds the Service Request bit to the data
//   and IDY frames passing it, so the HP-41 comes and reads the status.
//
// The front panel: the power switch is the device being switched on
// (enabled), the MODE switch (MAN / TRACE / NORM) is reported in the
// status, PRINT and PAPER ADVANCE are pressed and released from the view.
// A press is reported to the controller at least once, even if the finger
// was faster than the HP-41's next status read.
//
// What is printed: like the real printer, a line is a row of 168 dot
// columns (24 characters of 7 columns: a blank, the 5x7 character, a
// blank), built from characters (printer_font.h -- ASCII in escape mode,
// the HP-41's own set in 8-bit mode), graphics columns (column mode, ESC
// *b<n>G), skips, double wide and lower case, and printed -- kept in
// lines() -- at CR, when it's full, or right justified. Bar codes are
// stepped over (not drawn). The Printer view shows the paper.

#include "hpil.h"
#include "pico/time.h"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

enum class PrinterMode : std::uint8_t { Man = 0, Trace = 1, Norm = 2 };

class CPrinter : public CDevice {
public:
    static constexpr int kColumns = 24;          // characters per line
    static constexpr int kDots = 168;            // dot columns per line (24 x 7)
    static constexpr int kIntensities = 5;       // INTENSITY switch positions
    static constexpr std::size_t kMaxLines = 200; // lines kept (oldest dropped)

    // A printed line: its dot columns (bit 0 = top dot, bit 7 = the
    // eighth row graphics can use) and, for the log, its text
    struct Line {
        std::array<std::uint8_t, kDots> dots{};
        std::string text;
    };

    CPrinter(const char* name, IL_ADDR_t sai = 0x20, IL_ADDR_t aau = 1)
        : CDevice(name, sai, aau, PRINTER) {
        reset();
    }

    ViewKind viewKind() const override { return ViewKind::Printer; }

    // Adds the Service Request bit to passing frames while SR is set
    IL_CMD_t hpil(IL_CMD_t cmd) override;

    // One byte as if from the controller (the demo, demo.h)
    void put(std::uint8_t c) { feed(c); }
    // Tears off the paper: the kept lines are dropped (lineCount() stays)
    void clearPaper() { lines_.clear(); }
    void clear() override;                       // DCL / SDC
    void doListener(IL_CMD_t cmd, IL_CMD_t* rtn) override;
    void doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) override;
    void show() override;                        // extended trace

    // ── Front panel ───────────────────────────────────────────────────
    void setMode(PrinterMode m) { mode_ = m; }
    PrinterMode mode() const { return mode_; }
    static const char* modeName(PrinterMode m);
    // 0 (LIGHT) .. 4 (DARK)
    void setIntensity(std::uint8_t i) { intensity_ = i < kIntensities ? i : kIntensities - 1; }
    std::uint8_t intensity() const { return intensity_; }
    // PRINT / PAPER ADVANCE held down (true) or let go (false)
    void pressPrint(bool down);
    void pressAdvance(bool down);
    bool printDown() const { return printDown_; }
    bool advanceDown() const { return advanceDown_; }

    // Moves the paper one line (an empty line), as PAPER ADVANCE does --
    // without telling the controller (the view repeats it while the key
    // is held)
    void feedPaper();

    // ── What has been printed ─────────────────────────────────────────
    // The lines so far, oldest first (at most kMaxLines). A paper advance
    // is an empty line.
    const std::deque<Line>& lines() const { return lines_; }
    // Counts every line ever printed (also those dropped from lines())
    std::uint32_t lineCount() const { return lineCount_; }
    // The line with number n (counted like lineCount(), the first is 0),
    // nullptr if it's no longer kept (or not printed yet)
    const Line* line(std::uint32_t n) const;
    // Called for each new line (after it's added to lines())
    void setLineCallback(std::function<void(const Line&)> cb) { onLine_ = std::move(cb); }

    // The 16-bit status as described above (byte order: low byte first)
    std::uint16_t status() const;

private:
    // Status bits
    static constexpr std::uint16_t kSR = 0x40, kMB = 0x20, kMA = 0x10, kER = 0x08,
                                   kPA = 0x04, kPR = 0x02;
    static constexpr std::uint16_t kEL = 0x80 << 8, kID = 0x40 << 8, kBE = 0x20 << 8,
                                   kEB = 0x10 << 8, kRJ = 0x08 << 8, kDW = 0x04 << 8,
                                   kCO = 0x02 << 8, kLC = 0x01 << 8;
    // Mode bits set by the 8-bit codes and escape sequences
    static constexpr std::uint8_t kModeLower = 0x01, kModeColumn = 0x02, kModeDouble = 0x04,
                                  kModeRjust = 0x08, kModeInhibitAdv = 0x10;

    void reset();                    // power on / device clear
    void feed(std::uint8_t c);       // one byte from the controller
    void escape(const std::string& seq);
    void addChar(std::uint8_t c);    // a character: 7 columns (14 double wide)
    void addColumn(std::uint8_t bits);   // a dot column (2 double wide)
    void addBlank(int columns);      // skips
    bool room(int columns);          // fits on the line? (else prints the line first)
    void endOfLine();
    void addLine(Line&& line);

    PrinterMode mode_ = PrinterMode::Man;
    std::uint8_t intensity_ = 2;

    // Keys: held, and "seen by the controller" (a quick tap is still
    // reported once -- see pressPrint())
    bool printDown_ = false, advanceDown_ = false;
    bool printLatched_ = false, advanceLatched_ = false;
    absolute_time_t printLatchEnd_ = {}, advanceLatchEnd_ = {};
    bool srq_ = false;               // SR: cleared when the status is read

    // Talker: the two status bytes, then ETO
    int statusLeft_ = 0;             // bytes still to send after the first
    std::uint16_t statusSent_ = 0;   // the status being sent
    bool ownResponse_ = false;       // the frame just returned is our own answer

    // Character processing
    bool eightBit_ = false;          // ESC | ... 0xFC
    std::uint8_t modes_ = 0;         // kMode*
    bool inEscape_ = false;
    std::string escBuf_;
    int graphicsLeft_ = 0;           // ESC *b<n>G: n raw column bytes follow
    int barcodeLeft_ = 0;            // ESC *z<n>B / 8-bit 0x80+n: n bar code bytes follow
    Line building_;                  // the line being built
    int used_ = 0;                   // its columns so far

    std::deque<Line> lines_;
    std::uint32_t lineCount_ = 0;
    std::function<void(const Line&)> onLine_;
};

// The single CPrinter instance created in hipi_init() (hipi.cpp)
extern CPrinter* printer;

#endif//__PRINTER_H__
