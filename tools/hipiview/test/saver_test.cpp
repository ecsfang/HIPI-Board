// saver_test.cpp -- the screen saver's geese (saver_anim.cpp) on the PC,
// with the LT7683 driver against the emulated chip: the geese are kept in
// the display's SDRAM (LT7683::kGooseAddr) and copied to the panel by BTE,
// none in the MCU's RAM. Saves saver_geese.png.
//
//   make saver_test && ./saver_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "saver_anim.h"
#include "config.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
hipi::Config config;
namespace hipi {
int mirrorLayers = 0;
std::uint32_t mirrorLayerAddr = 0;
void displayMirror_addLayer(std::uint32_t addr, std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t) {
    ++mirrorLayers;
    mirrorLayerAddr = addr;
}
}

namespace {
LT7683Emu chip;
class ChipTransport : public hipi::DisplayTransport {
public:
    void csLow() override { first_ = true; }
    void csHigh() override {}
    void rstLow() override {}
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
    }
    void delayMs(std::uint32_t) override {}
    void delayUs(std::uint32_t) override {}
private:
    bool first_ = true;
    std::uint8_t type_ = 0;
};
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    const int heapBefore = mallinfo2().uordblks > 0 ? static_cast<int>(mallinfo2().uordblks) : 0;
    hipi::anim_start(hipi::AnimKind::Goose, &d, false);
    const int heapAfter = static_cast<int>(mallinfo2().uordblks);
    std::printf("     heap: %d bytes more after the geese were made\n", heapAfter - heapBefore);
    check(heapAfter - heapBefore < 2048, "the geese aren't kept in RAM (they were 36 kB)");
    check(hipi::mirrorLayers >= 1 && hipi::mirrorLayerAddr == hipi::LT7683::kGooseAddr, "their layer is told to the mirror");
    // Fly a while
    for (int i = 0; i < 20000; ++i) { fakeNowUs += 1000; hipi::anim_step(); }
    std::vector<std::uint32_t> px(1024 * 600);
    chip.render(px.data(), 0.0);
    writePng("saver_geese.png", px.data(), 1024, 600);
    std::printf("     saved saver_geese.png\n");
    int lit = 0;
    for (std::uint32_t p : px) if ((p & 0xFFFFFF) != 0) ++lit;
    std::printf("     %d pixels drawn\n", lit);
    check(lit > 1000, "geese on the screen");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
