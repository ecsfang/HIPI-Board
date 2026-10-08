// host_test.cpp -- runs HIPI's real LT7683 driver, Screen class, mirror
// encoder and mirror session on the PC against an emulated chip, and
// checks that a second emulator fed only with the mirror stream ends up
// showing exactly the same picture -- after joining mid-way (read-back
// sync) and while following live drawing afterwards.
//
//   make test        (in tools/hipiview)
#include "LT7683.hpp"
#include "Screen.hpp"
#include "mirror_encoder.hpp"
#include "mirror_session.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

LT7683Emu chip;     // stands in for the real controller
LT7683Emu viewer;   // what hipiview would show

// Everything sent is also saved as a hipiview log (test_stream.hvlog),
// framed like HIPI sends it, one record per piece, 1 ms apart -- for
// trying --replay / --render. With HV_DAMAGE=1 in the environment, a few
// bytes of one piece in the middle are left out of the log, to try how
// hipiview copes with damaged data.
class StreamSink : public hipi::mirror::Sink {
public:
    std::size_t total = 0;
    FILE* log = nullptr;
    std::uint32_t ms = 0;
    std::uint32_t damageAt = 0;
    bool put(const std::uint8_t* d, std::size_t n, bool) override {
        viewer.feed(d, n);
        total += n;
        if (log) {
            namespace mp = hipi::mirror;
            const std::uint16_t crc = mp::crc16(d, n);
            const std::uint8_t fh[mp::kFrameHeader] = { mp::kFrame0, mp::kFrame1,
                static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8) };
            std::size_t keep = n;
            if (damageAt && ms == damageAt && n > 40) keep = n - 37;          // lose a few bytes
            const std::uint32_t hdr[2] = { ms++, static_cast<std::uint32_t>(keep + sizeof fh) };
            std::fwrite(hdr, sizeof hdr, 1, log);
            std::fwrite(fh, 1, sizeof fh, log);
            std::fwrite(d, 1, keep, log);
        }
        return true;
    }
};
StreamSink sink;
hipi::mirror::Encoder encoder(sink);

// Feeds every SPI cycle to the emulated chip (answering reads like it)
// and to the mirror encoder, exactly like MirrorTransport does on HIPI
class LoopTransport : public hipi::RA8875Transport {
public:
    void csLow() override { first_ = true; encoder.onCsLow(); }
    void csHigh() override { encoder.onCsHigh(); }
    void rstLow() override { encoder.onReset(); }
    void rstHigh() override {}
    void spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) override {
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t b = tx ? tx[i] : 0;
            if (first_) { type_ = b & 0xC0; first_ = false; if (rx) rx[i] = 0; continue; }
            std::uint8_t r = 0;
            switch (type_) {
            case 0x00: chip.hostCmd(b); break;
            case 0x80: chip.hostWrite(b); break;
            case 0xC0: r = chip.hostRead(); break;
            default:   r = chip.hostStatus(); break;
            }
            if (rx) rx[i] = r;
        }
        encoder.onTransfer(tx, rx, len);
    }
    void delayMs(std::uint32_t) override {}
    void delayUs(std::uint32_t) override {}
private:
    bool first_ = true;
    std::uint8_t type_ = 0;
};

int failures = 0;

void compare(const char* what, const char* png) {
    const int W = 1024, H = 600;
    std::vector<std::uint32_t> a(W * H), b(W * H);
    chip.render(a.data(), 0.0);
    viewer.render(b.data(), 0.0);
    long diff = 0;
    int fx = -1, fy = -1;
    for (int i = 0; i < W * H; ++i)
        if (a[i] != b[i]) { if (diff == 0) { fx = i % W; fy = i / W; } ++diff; }
    writePng(std::string(png) + "_viewer.png", b.data(), W, H);
    if (diff) {
        writePng(std::string(png) + "_chip.png", a.data(), W, H);
        std::printf("FAIL %-28s %ld pixels differ (first at %d,%d)\n", what, diff, fx, fy);
        ++failures;
    } else {
        std::printf("ok   %-28s identical (stream so far %zu bytes)\n", what, sink.total);
    }
}

void compareMemory(const char* what, std::uint32_t addr, std::uint32_t len) {
    const auto& a = chip.memory();
    const auto& b = viewer.memory();
    std::uint32_t bad = 0, first = 0;
    for (std::uint32_t i = 0; i < len; ++i)
        if (a[addr + i] != b[addr + i]) { if (!bad) first = addr + i; ++bad; }
    if (bad) { std::printf("FAIL %-28s %u bytes differ (first at 0x%06X)\n", what, bad, first); ++failures; }
    else std::printf("ok   %-28s identical\n", what);
}

// A recognisable stand-in for the chip's built-in font: HIPI's own font
// for ASCII, a hollow box for everything else
void makeChipFont() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else for (int r = 0; r < 16; ++r) f[c][r] = (r == 2 || r == 13) ? 0x7E : (r > 2 && r < 13 ? 0x42 : 0);
    }
    chip.setBuiltinFont(f);
}

}  // namespace

int main() {
    sink.log = std::fopen("test_stream.hvlog", "wb");
    if (sink.log) std::fputs("HIPIVIEWLOG2\n", sink.log);
    makeChipFont();
    LoopTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    hipi::Screen screen(&d, 0x07E0, 1, 200, 1024);

    // ── Before the viewer connects ──────────────────────────────────────
    screen.pr_str("HIPI DISPLAY MIRROR TEST\r\nLine two: 0123456789 abcdefg\r\n");
    for (int i = 0; i < 30; ++i) {
        char line[64];
        std::snprintf(line, sizeof line, "Scroll line %02d of thirty\r\n", i);
        screen.pr_str(line);
    }
    d.fillRect(700, 40, 200, 100, 0xF800);
    d.fillCircle(800, 300, 60, 0x001F);
    d.roundRect(620, 420, 300, 120, 20, 0xFFE0);
    d.fillTriangle(100, 500, 300, 560, 160, 420, 0x07FF);
    // A picture in the Tape layer, not shown yet
    d.beginLayerDraw(hipi::LT7683::kTapeLayerAddr);
    d.fillRect(0, 0, 1024, 600, 0x4208);
    d.fillEllipse(512, 300, 300, 150, 0xFD20);
    d.endOverlayDraw();
    // An open "menu" in the PIP-1 layer, with the built-in font
    d.beginOverlayDraw();
    d.fillRoundRect(300, 150, 400, 260, 12, 0x2104);
    d.selectBuiltinFont();
    d.txtColor(0xFFFF, 0x2104);
    d.txtSize(1);
    d.txtSetCursor(330, 180);
    d.txtWrite("Menu: Hello \xC3\xA5\xC3\xA4\xC3\xB6");
    d.txtSize(0);
    d.txtSetCursor(330, 240);
    d.txtWrite("Small built-in text 123");
    d.endOverlayDraw();
    d.showPipOverlay(300, 150, 400, 260);
    d.selectCustomFont();

    // ── The viewer connects mid-way: session + read-back ────────────────
    // The viewer's asset cache (in memory here; hipiview keeps it on disk)
    std::map<std::uint32_t, std::vector<std::uint8_t>> cache;
    viewer.loadAsset = [&](std::uint32_t id, std::vector<std::uint8_t>& out) {
        auto it = cache.find(id);
        if (it == cache.end()) return false;
        out = it->second;
        return true;
    };
    viewer.saveAsset = [&](std::uint32_t id, const std::vector<std::uint8_t>& b) { cache[id] = b; };

    encoder.setEnabled(true);
    hipi::mirror::Session session(encoder);
    // Like HIPI at boot: the Tape picture is a fixed asset, the cassette
    // area a run-time layer
    {
        hipi::mirror::Layer tape;
        tape.addr = hipi::LT7683::kTapeLayerAddr; tape.rowBytes = 1024 * 600 * 2; tape.rows = 1;
        tape.assetId = 0x7A9E0001;
        session.addLayer(tape);
        hipi::mirror::Layer label;
        label.addr = hipi::LT7683::kCassetteLabelAddr; label.rowBytes = 200 * 2; label.rowStep = 2048; label.rows = 100;
        session.addLayer(label);
    }
    session.start(d);
    int steps = 0;
    while (!session.step(d, 4096)) { encoder.flush(); ++steps; }
    encoder.flush();
    std::printf("sync: %d steps, %zu bytes on the wire, font known: %s, errors %llu\n",
                steps, sink.total, viewer.fontKnown() ? "yes" : "no",
                static_cast<unsigned long long>(viewer.streamErrors()));
    compare("after sync", "t1_sync");
    compareMemory("panel memory", 0, 1024 * 600 * 2);
    compareMemory("tape layer", hipi::LT7683::kTapeLayerAddr, 1024 * 600 * 2);
    compareMemory("cgram", hipi::LT7683::kCgramAddr, 4096);

    // ── Live drawing after the sync ─────────────────────────────────────
    if (std::getenv("HV_DAMAGE")) sink.damageAt = sink.ms + 2;
    const std::size_t before = sink.total;
    screen.pr_str("Typed after the viewer connected\r\n");
    encoder.flush();
    std::printf("one line of text: %zu bytes\n", sink.total - before);
    compare("live text", "t2_text");

    d.hidePipOverlay();
    d.copyLayerToPanel(hipi::LT7683::kTapeLayerAddr, 0, 300, 1024, 300);
    encoder.flush();
    compare("menu closed + layer copy", "t3_copy");

    static std::uint16_t bmp[64 * 48];
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x)
        bmp[y * 64 + x] = static_cast<std::uint16_t>(((x * 31 / 63) << 11) | ((y * 63 / 47) << 5) | 0x10);
    d.drawBitmap565(20, 20, 64, 48, bmp);
    d.bteMcuWriteBitmap(100, 20, 64, 48, 64, bmp);
    d.bteScrollShift(0, 0, 1024, 300, 16);
    d.line(0, 0, 1023, 599, 0xFFFF);
    d.ellipse(512, 300, 200, 100, 0xF81F);
    d.triangle(10, 10, 200, 50, 50, 200, 0xFFFF);
    d.showPipWindow(2, hipi::LT7683::kTapeLayerAddr, 1024, 400, 100, 600, 50, 200, 120);
    encoder.flush();
    compare("bitmaps, BTE, shapes, PIP-2", "t4_misc");

    std::printf("assets cached by the viewer: %zu\n", cache.size());

    // A second session (reconnect), with the Tape picture already cached:
    // it must come out right, with the picture only named, not sent
    encoder.setEnabled(false);
    encoder.setEnabled(true);
    std::vector<std::uint32_t> have;
    for (auto& c : cache) have.push_back(c.first);
    session.setViewerAssets(have.data(), static_cast<int>(have.size()));
    const std::size_t before2 = sink.total;
    session.start(d);
    while (!session.step(d, 4096)) encoder.flush();
    encoder.flush();
    std::printf("second session: %zu bytes on the wire\n", sink.total - before2);
    compare("second session (cached)", "t5_resync");
    compareMemory("tape layer from the cache", hipi::LT7683::kTapeLayerAddr, 1024 * 600 * 2);

    if (sink.log) std::fclose(sink.log);
    std::printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
