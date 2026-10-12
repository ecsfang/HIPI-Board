// printertest -- TFPRINT (src/printer.cpp) against the frames an HP-41
// sends it. The loop is just the printer: what it returns is what the
// controller gets back. See the Makefile.
#include "printer.h"
#include <cstdio>
#include <string>

bool bTrace = false;
bool bExtTrace = false;
bool usb_connected = true;
std::uint64_t fakeNowUs = 1000000;

static int failures = 0, checks = 0;

static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

static void checkEq(unsigned got, unsigned want, const char* what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("FAIL: %s: got 0x%03X, want 0x%03X\n", what, got, want);
    }
}

static IL_CMD_t send(CPrinter& p, IL_CMD_t f) { return p.hpil(f); }

static void sendText(CPrinter& p, const std::string& s) {
    for (unsigned char c : s) {
        const IL_CMD_t r = send(p, c);
        if (r != c) { ++failures; std::printf("FAIL: data 0x%02X came back as 0x%03X\n", c, r); }
    }
}

// Reads the status as the HP-41 does: TAD, SST, the bytes, UNT
static std::uint16_t readStatus(CPrinter& p) {
    send(p, TAD + 1);
    const IL_CMD_t lo = send(p, SST);
    const IL_CMD_t hi = send(p, lo);          // our byte back round the loop
    const IL_CMD_t end = send(p, hi);
    checkEq(end, ETO, "status ends with ETO");
    check(!(lo & SRQ_BIT) && !(hi & SRQ_BIT), "status bytes carry no SRQ");
    send(p, UNT);
    return static_cast<std::uint16_t>((lo & 0xFF) | ((hi & 0xFF) << 8));
}

static std::string lastLine(const CPrinter& p) { return p.lines().empty() ? "<none>" : p.lines().back().text; }
static const CPrinter::Line& last(const CPrinter& p) { return p.lines().back(); }

static void checkLine(const CPrinter& p, const std::string& want, const char* what) {
    ++checks;
    if (lastLine(p) != want) {
        ++failures;
        std::printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, lastLine(p).c_str(), want.c_str());
    }
}

int main() {
    CPrinter p("TFPRINT");

    // ── Addressing and identification ────────────────────────────────
    checkEq(send(p, AAU), AAU, "AAU passes");
    checkEq(send(p, AAD + 1), AAD + 2, "AAD: takes address 1");
    checkEq(p.addr(), 1, "address");
    send(p, TAD + 1);
    const IL_CMD_t aid = send(p, SAI);
    checkEq(aid, 0x20, "accessory ID: printer");
    checkEq(send(p, aid), ETO, "SAI ends with ETO");
    send(p, UNT);

    // ── Status ───────────────────────────────────────────────────────
    std::uint16_t st = readStatus(p);
    checkEq(st & 0xFF, 0x00, "status low: MAN, nothing pressed");
    checkEq(st >> 8, 0xE0, "status high: EL, ID, BE");
    p.setMode(PrinterMode::Norm);
    checkEq(readStatus(p) & 0xFF, 0x20, "NORM -> MB");
    p.setMode(PrinterMode::Trace);
    checkEq(readStatus(p) & 0xFF, 0x10, "TRACE -> MA");
    p.setMode(PrinterMode::Man);
    // NRD after the first byte: ETO on its way back
    send(p, TAD + 1);
    const IL_CMD_t lo = send(p, SST);
    checkEq(send(p, NRD), NRD, "NRD passes");
    checkEq(send(p, lo), ETO, "NRD: ETO instead of the second byte");
    send(p, UNT);
    send(p, TAD + 1);
    checkEq(send(p, SDA), ETO, "SDA: nothing to send");
    send(p, UNT);

    // ── Text ─────────────────────────────────────────────────────────
    send(p, LAD + 1);
    sendText(p, "HELLO\r\n");
    checkLine(p, "HELLO", "plain line, LF ignored");
    {
        const auto& d = last(p).dots;
        check(d[0] == 0 && d[1] == 0x7F && d[2] == 0x08 && d[5] == 0x7F && d[6] == 0, "'H' as dots, in its 7 columns");
        check(d[35] == 0 && d[36] == 0, "the rest of the line blank");
    }
    sendText(p, "\x1b*b3G" "ABC" "X\r");
    checkLine(p, "X", "graphics bytes after ESC *b3G skipped");
    sendText(p, "\x1b&k1S" "\x1b&k0S" "Y\r");
    checkLine(p, "Y", "escape sequences skipped");
    sendText(p, "\x80" "Q\r");
    checkLine(p, "Q", "8-bit code ignored outside 8-bit mode");
    sendText(p, "\x1b|" "\xA3" "Z\r");
    checkLine(p, "   Z", "8-bit: skip 3 characters");
    check((readStatus(p) >> 8) & 0x10, "8-bit mode in the status (EB)");
    send(p, LAD + 1);
    sendText(p, "\xD1" "ABC\r" "\xD0");
    checkLine(p, "abc", "8-bit: lower case");
    sendText(p, "\xE8" "R\r" "\xE0");
    checkLine(p, std::string(23, ' ') + "R", "8-bit: right justified");
    sendText(p, std::string("\x00", 1) + "\r");
    check(last(p).dots[1] == 0x08 && last(p).dots[3] == 0x3E, "8-bit mode: code 0 is the HP-41 diamond");
    sendText(p, "\xD4" "ABCDEFGHIJKLM" "\r" "\xD0");
    checkLine(p, "M", "double wide: 12 per line, the 13th on the next");
    check(last(p).dots[2] == 0x7F && last(p).dots[3] == 0x7F && last(p).dots[4] == 0x02, "double wide: each column twice");
    sendText(p, "\xD2" "\x7F\x41\r" "\xD0" "C\r");
    checkLine(p, "C", "column mode: bytes are columns, CR too");
    {
        const auto& d = last(p).dots;
        check(d[0] == 0x7F && d[1] == 0x41 && d[2] == 0x0D, "column mode: the bytes are the dot columns");
        check(d[3] == 0 && d[4] == 0x3E && d[8] == 0x22 && d[9] == 0, "then 'C' from the HP-41 set");
    }
    sendText(p, "\x82" "\x01\x02" "B\r");
    checkLine(p, "B", "8-bit bar code: its bytes skipped");
    sendText(p, "ABCDEFGHIJKLMNOPQRSTUVWX" "Y\r");
    checkLine(p, "Y", "24 characters print by themselves");
    sendText(p, "\xFC" "\xA3" "K\r");
    checkLine(p, "K", "0xFC: back from 8-bit mode");
    sendText(p, "\x1b*b2G" "\xFF\x81" "\r");
    check(last(p).dots[0] == 0xFF && last(p).dots[1] == 0x81, "ESC *b2G: two graphics columns, all 8 rows");
    check(p.line(p.lineCount() - 1) == &last(p) && p.line(p.lineCount()) == nullptr, "line(n)");
    const std::uint32_t before = p.lineCount();
    sendText(p, "PARTIAL");
    checkEq(readStatus(p) >> 8, 0x40, "a line being built: not EL, not BE");
    send(p, DCL);
    checkEq(readStatus(p) >> 8, 0xE0, "DCL: line dropped, status as at start");
    checkEq(p.lineCount(), before, "DCL prints nothing");

    // ── PRINT key: a quick tap is still reported once ────────────────
    p.pressPrint(true);
    p.pressPrint(false);
    checkEq(send(p, IDY | 0xC0), (IDY | 0xC0) | SRQ_BIT, "PRINT: SRQ on IDY");
    send(p, LAD + 5);                           // someone else listens
    checkEq(send(p, 'A'), 'A' | SRQ_BIT, "PRINT: SRQ on passing data");
    checkEq(send(p, UNL), UNL, "no SRQ on commands");
    st = readStatus(p);
    checkEq(st & 0xFF, 0x02, "status: PR (SR is cleared when read)");
    checkEq(send(p, IDY | 0xC0), IDY | 0xC0, "after the status read: no SRQ");
    checkEq(readStatus(p) & 0xFF, 0x00, "PR reported once");
    // Held: reported as long as it's down
    p.pressPrint(true);
    readStatus(p);
    checkEq(readStatus(p) & 0xFF, 0x02, "PR while held");
    p.pressPrint(false);
    checkEq(readStatus(p) & 0xFF, 0x00, "PR gone when let go");
    // Nobody comes: forgotten after a while
    p.pressPrint(true);
    p.pressPrint(false);
    fakeNowUs += 3100000;
    checkEq(send(p, IDY | 0xC0), IDY | 0xC0, "an old tap: SRQ withdrawn");
    checkEq(readStatus(p) & 0xFF, 0x00, "an old tap: not reported");

    // ── PAPER ADVANCE: a blank line, PA ──────────────────────────────
    const std::uint32_t n = p.lineCount();
    p.pressAdvance(true);
    checkEq(p.lineCount(), n + 1, "ADV: one blank line");
    checkLine(p, "", "ADV: the line is blank");
    p.pressAdvance(false);
    checkEq(readStatus(p) & 0xFF, 0x04, "status: PA");

    // ── Off the loop ─────────────────────────────────────────────────
    send(p, AAU);
    checkEq(p.addr(), 1, "AAU: back to address 1");

    std::printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
