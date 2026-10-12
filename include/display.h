#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <cstdint>
#include <queue>
#include "hpil.h"

namespace hipi { class Video80; }

// TFDISPLAY -- the video display. It emulates one of two (Settings ->
// Screen -> Emulate, config display_model):
//   HP 82163 Video Interface: accessory ID 0x3E, text through Screen
//     (Screen.hpp) -- 32 x 16 with HIPI's font size / columns settings.
//   HP 92198 (Mountain Computer MC00701A) 80-Column Video Interface:
//     accessory ID 0x32 ($32 = 50), device ID "MC00701A" CR LF, SST = $80
//     and the device dependent status byte (Video80::status()) then ETO;
//     text through Video80 (video80.h).
// Either way: address 31 after AAU, DCL/SDC clears, and the data bytes
// are queued and handed over one per idle().
enum class DisplayModel : std::uint8_t { Hp82163 = 0, Hp92198 = 1 };

class CDisplay : public CDevice {
    std::queue<unsigned char> fifo;
    std::uint32_t totalPushed_ = 0;  // see totalPushed()'s own comment
    DisplayModel model_ = DisplayModel::Hp82163;
    hipi::Video80* video80_ = nullptr;
    int statusLeft_ = 0;              // 92198 SST: bytes still to come
    std::uint8_t statusByte_ = 0;
public:
    static constexpr IL_ADDR_t kSai82163 = 0x3E;
    static constexpr IL_ADDR_t kSai92198 = 0x32;

    ViewKind viewKind() const override { return ViewKind::Display; }
    CDisplay(const char *name, IL_ADDR_t _sai = kSai82163, IL_ADDR_t _aau=31) : CDevice(name, _sai, _aau, DISPLAY) {
    }
    // The 92198's screen (needed before setModel(Hp92198))
    void setVideo80(hipi::Video80* v) { video80_ = v; }
    hipi::Video80* video80() const { return video80_; }
    // Which display it is. Changing it empties the queue; the controller
    // sees the new identity the next time it asks (AUTOIO / power cycle).
    void setModel(DisplayModel m);
    DisplayModel model() const { return model_; }
    bool isVideo80() const { return model_ == DisplayModel::Hp92198 && video80_ != nullptr; }
    const char* deviceId() override;
    void doListener(IL_CMD_t cmd, IL_CMD_t *rtn);
    void doTalker(IL_CMD_t cmd, IL_CMD_t *rtn) override;
    void show(void);
    void clear(void);
    void idle(void);
    void ifc(void);
    // Exposes fifo's current size for pico_main.cpp's own periodic
    // diagnostic logging -- doListener() pushes into this queue whenever
    // HP-IL data arrives, but idle() only pops (and processes via
    // Screen::pr_char()) ONE byte per call, and only during genuinely
    // idle main-loop iterations (no HP-IL frame received that pass) --
    // if data arrives faster than that drains it, this queue has no
    // upper bound at all and could grow without limit, a completely
    // separate potential memory-growth source from the one already fixed
    // in Screen's own line buffer.
    std::size_t fifoSize() const { return fifo.size(); }
    // Running total of bytes ever pushed into fifo (never reset) --
    // compare against Screen::charsDrawn() to see whether drawing keeps
    // up with reception or silently falls behind/stops partway through
    // a long text stream.
    std::uint32_t totalPushed() const { return totalPushed_; }
};

extern CDisplay* videoDisplay;   // Same pattern as pilbox.h's `extern CPilBox* pilbox;`

#endif//__DISPLAY_H__
