// pixeltest -- TFPIXEL on the PC (see Makefile): pixel count, effects and
// the current limit, with the real PicoLED library; PIO and clock stood in
#include "pixels.h"
#include <cstdio>
#include <cstring>
#include <string>

uint64_t g_fakeUs = 1000000;
pio_hw_t g_pio0{0}, g_pio1{1};
std::vector<uint32_t> g_wire;
bool bTrace = true, bExtTrace = false;
extern CPixelParser pixelParser;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void send(const std::string& s) { for (unsigned char c : s) pixelParser.feed(c); }
static void run(int ms) { for (int t = 0; t < ms; t += 5) { g_fakeUs += 5000; pixelStrip.poll(); } }
// The last frame on the wire (32 pixels), as 0xRRGGBB (GRB order on the wire, <<8)
static std::vector<uint32_t> frame() {
    std::vector<uint32_t> f(g_wire.end() - 32, g_wire.end());
    for (auto& v : f) { const uint32_t g = (v >> 24) & 0xFF, r = (v >> 16) & 0xFF, b = (v >> 8) & 0xFF; v = (r << 16) | (g << 8) | b; }
    return f;
}
static int lit(const std::vector<uint32_t>& f, int a, int b) { int n = 0; for (int i = a; i <= b; ++i) n += f[i] != 0; return n; }

int main() {
    int saved = -1;
    pixelStrip.setCountChangedCallback([&](uint32_t n) { saved = static_cast<int>(n); });
    CHECK(pixelStrip.count() == 5, "initial count %u", pixelStrip.count());

    send("0N8\r\n");
    CHECK(pixelStrip.count() == 8 && saved == 8, "count %u saved %d", pixelStrip.count(), saved);

    send(std::string("0X") + char(0xE0) + "\r\n");          // all red
    auto f = frame();
    CHECK(lit(f, 0, 7) == 8 && lit(f, 8, 31) == 0, "red on 8: %d, beyond %d", lit(f, 0, 7), lit(f, 8, 31));

    send("1-5ES255,252:4\r\n");
    send("6-8EC224:12\r\n");
    run(3000);
    f = frame();
    CHECK(lit(f, 8, 31) == 0, "nothing beyond 8");
    CHECK(lit(f, 5, 7) >= 1, "comet visible on 6-8");

    // A static colour inside the stars: they stop, pixel 3 blue
    send(std::string("3X") + char(0x03) + "\r\n");
    run(500);
    f = frame();
    CHECK(f[2] >= 0x0000FE && f[2] <= 0x0000FF, "pixel 3 blue: %06X", f[2]);
    CHECK(f[0] == 0 && f[1] == 0 && f[3] == 0 && f[4] == 0, "stars' other pixels off");
    CHECK(lit(f, 5, 7) >= 1, "comet still running");

    // Every effect runs a while without trouble
    const char* all[] = { "0EC", "0EM", "0ES", "0EB", "0EF", "0EO", "0ER", "0EM224,28:1:-5:2", "0EB5:255:50:2",
                          "0EF10:3:2", "0ER200:30", "0EC3:1000:8:50", "0ES:100:50" };
    for (const char* e : all) { send(std::string(e) + "\r\n"); run(2000); }
    send("0ER\r\n"); run(200); f = frame();
    CHECK(lit(f, 0, 7) == 8, "rainbow on all 8 (%d)", lit(f, 0, 7));
    send("1-2EF\r\n");      // fire on 2 pixels: refused, rainbow keeps running
    run(100); f = frame(); CHECK(lit(f, 0, 7) == 8, "rainbow untouched");

    // Four at once, a fifth replaces the oldest
    send("0E\r\n"); send("1-2ES 3-4EC 5-6EM 7-8ER\r\n"); run(1000);
    send("1-1EB\r\n"); run(1000);   // overlaps the first: replaces it
    f = frame(); CHECK(lit(f, 0, 7) >= 4, "several effects running (%d lit)", lit(f, 0, 7));

    // Fewer pixels: the effects beyond stop, the pixels go dark
    send("0N4\r\n"); run(500); f = frame();
    CHECK(lit(f, 4, 31) == 0 && saved == 4, "beyond 4 dark (%d), saved %d", lit(f, 4, 31), saved);
    send("0E\r\n"); run(100); f = frame(); CHECK(lit(f, 0, 31) == 0, "all stopped and dark");

    // Current limit: 32 white pixels are limited
    send("0N32\r\n");
    send(std::string("0X") + char(0xFF) + "\r\n");
    f = frame();
    unsigned level = f[0] & 0xFF;
    CHECK(level > 30 && level < 80, "32 white pixels limited to %u of 255", level);
    send("0N5\r\n"); send(std::string("0X") + char(0xFF) + "\r\n"); f = frame();
    CHECK((f[0] & 0xFF) >= 254, "5 white pixels: full (%u)", f[0] & 0xFF);
    send("0S50\r\n"); f = frame(); CHECK((f[0] & 0xFF) >= 125 && (f[0] & 0xFF) <= 128, "brightness 50%%: %u", f[0] & 0xFF);

    // flush() ends a command the transmission didn't
    send("0S100\n0EC"); pixelParser.flush(); run(300); f = frame();
    CHECK(lit(f, 0, 4) >= 1, "comet started by flush");
    send("0E"); pixelParser.flush(); run(100); f = frame();
    CHECK(lit(f, 0, 31) == 0, "stopped by flush");

    // The colour byte order (pixel_order): the first byte on the wire
    pixelStrip.stopAllEffects();
    pixelStrip.clear();
    pixelStrip.setBrightness(100);
    pixelStrip.setPixelRGB(0, 200, 0, 0);                    // red
    pixelStrip.show();
    auto first = [](int byte) { return (g_wire[g_wire.size() - 32] >> (24 - 8 * byte)) & 0xFF; };
    CHECK(pixelStrip.colorOrder() == 0 && first(0) > 0 && first(1) == 0, "RGB: red first (%u %u)", first(0), first(1));
    pixelStrip.setColorOrder(pixelOrderIndex("grb"));
    pixelStrip.show();
    CHECK(pixelStrip.colorOrder() == 1 && first(0) == 0 && first(1) > 0, "GRB: red second (%u %u)", first(0), first(1));
    pixelStrip.setColorOrder(pixelOrderIndex("BRG"));
    pixelStrip.show();
    CHECK(first(0) == 0 && first(1) > 0 && first(2) == 0, "BRG: red second (%u %u %u)", first(0), first(1), first(2));
    pixelStrip.setColorOrder(pixelOrderIndex("GBR"));
    pixelStrip.show();
    CHECK(first(2) > 0 && first(0) == 0, "GBR: red third (%u %u %u)", first(0), first(1), first(2));
    CHECK(pixelOrderIndex("xyz") == -1 && pixelOrderIndex("RGBW") == -1, "unknown orders refused");
    pixelStrip.setColorOrder(0);

    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
