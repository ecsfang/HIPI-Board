// printout.cpp -- see printout.h
#include "printout.h"
#include "sd_paths.h"
#include "ff.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hipi {

namespace {

constexpr int kScale = 2;                       // pixels per dot
constexpr int kDotRows = 8;                     // dot rows a line can use
constexpr int kLineDots = 11;                   // a line's height: 8 dot rows + a gap
constexpr int kMargin = 8;                      // white around it, pixels
constexpr int kWidth = CPrinter::kDots * kScale + 2 * kMargin;     // 352
constexpr int kRowBytes = ((kWidth + 31) / 32) * 4;                // 1 bpp, 4-byte rows

void le16(std::uint8_t* p, std::uint32_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
void le32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }

bool exists(const char* path) {
    FILINFO info;
    return f_stat(path, &info) == FR_OK;
}

bool writeAll(FIL& f, const void* data, UINT n) {
    UINT bw = 0;
    return f_write(&f, data, n, &bw) == FR_OK && bw == n;
}

// The paper's lines, one at a time from the printer's store (thousands of
// them don't fit in RAM at once)
bool writeBmp(const char* path, const CPrinter& printer) {
    const std::uint32_t first = printer.firstKept();
    const int count = static_cast<int>(printer.keptLines());
    const int height = count * kLineDots * kScale + 2 * kMargin;
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    // Header: file, info (40 bytes), two colours (index 0 white, 1 black)
    std::uint8_t h[62] = {};
    h[0] = 'B'; h[1] = 'M';
    le32(h + 2, 62u + static_cast<std::uint32_t>(kRowBytes * height));
    le32(h + 10, 62);
    le32(h + 14, 40);
    le32(h + 18, kWidth);
    le32(h + 22, static_cast<std::uint32_t>(height));
    le16(h + 26, 1);
    le16(h + 28, 1);                                // 1 bit per pixel
    le32(h + 34, static_cast<std::uint32_t>(kRowBytes * height));
    le32(h + 38, 2835); le32(h + 42, 2835);         // 72 dpi
    le32(h + 46, 2);
    h[54] = 0xFF; h[55] = 0xFF; h[56] = 0xFF;       // 0: white
                                                    // 1: black (zeros)
    bool ok = writeAll(f, h, sizeof(h));
    // Rows bottom-up: the last line first (gathered, written 4 kB at a time)
    std::vector<std::uint8_t> row(kRowBytes);
    std::vector<std::uint8_t> out;
    out.reserve(4096 + kRowBytes);
    CPrinter::Line l;
    int have = -1;                                  // the line in `l`
    for (int y = height - 1; y >= 0 && ok; --y) {
        std::fill(row.begin(), row.end(), 0);
        const int py = y - kMargin;
        if (py >= 0 && py < count * kLineDots * kScale) {
            const int li = py / (kLineDots * kScale);
            if (li != have) { printer.readLine(first + static_cast<std::uint32_t>(li), l); have = li; }
            const int dotRow = (py % (kLineDots * kScale)) / kScale;
            if (dotRow < kDotRows) {
                for (int c = 0; c < CPrinter::kDots; ++c) {
                    if (!(l.dots[static_cast<std::size_t>(c)] >> dotRow & 1)) continue;
                    for (int k = 0; k < kScale; ++k) {
                        const int x = kMargin + c * kScale + k;
                        row[static_cast<std::size_t>(x / 8)] |= static_cast<std::uint8_t>(0x80 >> (x % 8));
                    }
                }
            }
        }
        out.insert(out.end(), row.begin(), row.end());
        if (out.size() >= 4096) { ok = writeAll(f, out.data(), static_cast<UINT>(out.size())); out.clear(); }
    }
    if (ok && !out.empty()) ok = writeAll(f, out.data(), static_cast<UINT>(out.size()));
    ok = f_close(&f) == FR_OK && ok;
    if (!ok) f_unlink(path);
    return ok;
}

bool writeTxt(const char* path, const CPrinter& printer) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    bool ok = true;
    CPrinter::Line l;
    for (std::uint32_t n = printer.firstKept(); n < printer.lineCount() && ok; ++n) {
        if (!printer.readLine(n, l)) continue;
        std::string s;
        for (char ch : l.text) {
            const auto c = static_cast<unsigned char>(ch);
            s += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (s.empty() && std::any_of(l.dots.begin(), l.dots.end(), [](std::uint8_t d) { return d != 0; }))
            s = "[graphics]";
        s += "\r\n";
        ok = ok && writeAll(f, s.data(), static_cast<UINT>(s.size()));
    }
    ok = f_close(&f) == FR_OK && ok;
    if (!ok) f_unlink(path);
    return ok;
}

bool readAll(FIL& f, void* data, UINT n) {
    UINT br = 0;
    return f_read(&f, data, n, &br) == FR_OK && br == n;
}

void paths(unsigned n, char* bmp, char* txt, std::size_t size) {
    std::snprintf(bmp, size, HIPI_DIR_PRINTOUTS "/printout_%u.bmp", n);
    std::snprintf(txt, size, HIPI_DIR_PRINTOUTS "/printout_%u.txt", n);
}

// The picture's lines (its height), -1 if it isn't one of ours
int bmpLines(const char* path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    std::uint8_t h[62];
    const bool ok = readAll(f, h, sizeof(h));
    f_close(&f);
    if (!ok || h[0] != 'B' || h[1] != 'M') return -1;
    const std::uint32_t w = h[18] | h[19] << 8 | h[20] << 16 | static_cast<std::uint32_t>(h[21]) << 24;
    const std::uint32_t ht = h[22] | h[23] << 8 | h[24] << 16 | static_cast<std::uint32_t>(h[25]) << 24;
    if (w != static_cast<std::uint32_t>(kWidth) || h[28] != 1 || ht < 2u * kMargin) return -1;
    return static_cast<int>((ht - 2 * kMargin) / (kLineDots * kScale));
}

}  // namespace

std::string printout_name(unsigned number) { return "printout_" + std::to_string(number); }

std::vector<PrintoutInfo> printout_list() {
    std::vector<PrintoutInfo> v;
    DIR dir;
    if (f_opendir(&dir, HIPI_DIR_PRINTOUTS) != FR_OK) return v;
    FILINFO info;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
        unsigned n = 0;
        char ext[8] = "";
        if (std::sscanf(info.fname, "printout_%u.%3s", &n, ext) != 2) continue;
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::strcmp(ext, "bmp") != 0 && std::strcmp(ext, "txt") != 0) continue;
        if (std::any_of(v.begin(), v.end(), [n](const PrintoutInfo& p) { return p.number == n; })) continue;
        v.push_back({ n, -1 });
    }
    f_closedir(&dir);
    for (PrintoutInfo& p : v) {
        char bmp[48], txt[48];
        paths(p.number, bmp, txt, sizeof(bmp));
        p.lines = bmpLines(bmp);
    }
    std::sort(v.begin(), v.end(), [](const PrintoutInfo& a, const PrintoutInfo& b) { return a.number > b.number; });
    return v;
}

bool printout_load(unsigned number, CPrinter& printer, std::string& message) {
    char bmp[48], txt[48];
    paths(number, bmp, txt, sizeof(bmp));
    const int n = bmpLines(bmp);
    if (n < 0) { message = "Can't read " + printout_name(number); return false; }
    FIL f;
    if (f_open(&f, bmp, FA_READ) != FR_OK) { message = "Can't read " + printout_name(number); return false; }
    const int height = n * kLineDots * kScale + 2 * kMargin;
    // Straight onto the paper, a line at a time (the file's rows go from
    // the last line up -- each line's rows together): a printout of
    // thousands of lines doesn't fit in RAM. Read wrong: the paper empty.
    const std::uint32_t first = printer.startPaper(static_cast<std::uint32_t>(n));
    std::vector<std::uint8_t> row(kRowBytes);
    CPrinter::Line l;
    int have = -1;                                  // the line in `l`
    bool ok = f_lseek(&f, 62) == FR_OK;
    // Rows bottom-up; one dot row is the first of its 2 pixel rows
    for (int y = height - 1; y >= 0 && ok; --y) {
        ok = readAll(f, row.data(), kRowBytes);
        const int py = y - kMargin;
        if (!ok || py < 0 || py >= n * kLineDots * kScale || py % kScale != 0) continue;
        const int dotRow = (py % (kLineDots * kScale)) / kScale;
        if (dotRow >= kDotRows) continue;
        const int li = py / (kLineDots * kScale);
        if (li != have) {
            if (have >= 0) printer.setLine(first + static_cast<std::uint32_t>(have), l);
            l = CPrinter::Line();
            have = li;
        }
        for (int c = 0; c < CPrinter::kDots; ++c) {
            const int x = kMargin + c * kScale;
            if (row[static_cast<std::size_t>(x / 8)] & (0x80 >> (x % 8)))
                l.dots[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(l.dots[static_cast<std::size_t>(c)] | (1 << dotRow));
        }
    }
    if (have >= 0) printer.setLine(first + static_cast<std::uint32_t>(have), l);
    f_close(&f);
    if (!ok) { printer.clearPaper(); message = "Can't read " + printout_name(number); return false; }
    // The text, if the file is there (for the trace log, and saving
    // again): read a piece at a time, each line put to its paper line
    if (f_open(&f, txt, FA_READ) == FR_OK) {
        std::string t;
        std::uint32_t i = 0;
        char buf[256];
        UINT br = 0;
        auto put = [&] {
            if (!t.empty() && t.back() == '\r') t.pop_back();
            if (t != "[graphics]" && printer.readLine(first + i, l)) {
                l.text = t;
                printer.setLine(first + i, l);
            }
            ++i;
            t.clear();
        };
        while (i < static_cast<std::uint32_t>(n) && f_read(&f, buf, sizeof(buf), &br) == FR_OK && br > 0) {
            for (UINT k = 0; k < br && i < static_cast<std::uint32_t>(n); ++k) {
                if (buf[k] == '\n') put();
                else if (t.size() < CPrinter::kMaxText + 1) t += buf[k];
            }
        }
        if (!t.empty() && i < static_cast<std::uint32_t>(n)) put();
        f_close(&f);
    }
    message = printout_name(number);
    return true;
}

bool printout_delete(unsigned number) {
    char bmp[48], txt[48];
    paths(number, bmp, txt, sizeof(bmp));
    const bool a = f_unlink(bmp) == FR_OK, b = f_unlink(txt) == FR_OK;
    return a || b;
}

bool printout_save(const CPrinter& printer, std::string& message) {
    if (printer.keptLines() == 0) { message = "Nothing on the paper"; return false; }
    f_mkdir(HIPI_DIR_PRINTOUTS);                    // (FR_EXIST if it's there)
    char bmp[48], txt[48];
    unsigned n = 1;
    for (; n < 10000; ++n) {
        std::snprintf(bmp, sizeof(bmp), HIPI_DIR_PRINTOUTS "/printout_%u.bmp", n);
        std::snprintf(txt, sizeof(txt), HIPI_DIR_PRINTOUTS "/printout_%u.txt", n);
        if (!exists(bmp) && !exists(txt)) break;
    }
    if (!writeBmp(bmp, printer)) { message = "Saving failed (SD card)"; return false; }
    if (!writeTxt(txt, printer)) { f_unlink(bmp); message = "Saving failed (SD card)"; return false; }
    char m[32];
    std::snprintf(m, sizeof(m), "printout_%u", n);
    message = m;
    return true;
}

}  // namespace hipi
