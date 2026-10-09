// display_mirror.h -- mirrors the 7" display to a PC over a fourth USB
// serial port ("HIPI Display Mirror", CDC3).
//
// Everything the firmware sends to the LT7683 display controller goes
// through MirrorTransport, which passes it on to the real SPI transport
// unchanged and, while a PC program has the mirror port open, also
// streams it to the PC (protocol: mirror_protocol.h). The PC program,
// tools/hipiview, runs an emulated LT7683 on that stream and shows -- and
// can record -- exactly what is on the panel: text, plots, menus, the
// screen saver, everything, including whatever is added to the firmware
// later, since it works at the controller's register level.
//
// When the viewer opens the port and asks for it (it sends "HIPIVIEW"),
// a session starts: a greeting, the current
// register values, the controller's built-in font, and then the SDRAM
// contents (the panel and the off-screen layers) are read back in small
// pieces from the main loop -- a few seconds, with HIPI working normally
// meanwhile. When the PC closes the port, mirroring stops and costs
// nothing.
//
// If the PC can't keep up, drawing waits for the USB port (up to a
// second); after that the session is restarted from scratch.
#pragma once
#include "display_config.h"
#include "RA8875Transport.hpp"
#include "hipi_features.h"      // HIPI_MIRROR_PORT / HIPI_MIRROR_TRANSPORT
#include <cstdint>
#include <functional>

namespace hipi {

// Wraps the real transport; give this to the display driver instead
class MirrorTransport : public RA8875Transport {
public:
    explicit MirrorTransport(RA8875Transport& inner);

    void csLow() override;
    void csHigh() override;
    void rstLow() override;
    void rstHigh() override;
    void spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) override;
    void delayMs(std::uint32_t ms) override { inner_.delayMs(ms); }
    void delayUs(std::uint32_t us) override { inner_.delayUs(us); }

private:
    RA8875Transport& inner_;
};

// The display the mirror reads back from (once, after the display is up)
void displayMirror_init(DisplayDriver* display);
// Main loop: starts/stops sessions, sends the read-back, flushes to USB
void displayMirror_poll();
// A PC viewer is connected and being fed
bool displayMirror_active();
// The setting (Settings > Screen > Mirror to PC, config display_mirror):
//   Off      the port stays silent and a running session ends at once
//            (the default -- feeding a viewer costs the display some speed)
//   View     the viewer shows the screen
//   Control  ... and can also work the touch screen and the buttons
enum class MirrorMode : std::uint8_t { Off = 0, View = 1, Control = 2 };
void displayMirror_setMode(MirrorMode m);
MirrorMode displayMirror_mode();
const char* displayMirror_modeName(MirrorMode m);    // "Off", "View", "Control"

// Input from the viewer (Control): where it goes. touch(down, x, y) is the
// mouse as a finger on the panel; button(code, down) a button, code as in
// mirror_protocol.h (1 Shift .. 5 X, 6/7 next/previous view). When the
// session ends or Control is switched off: touch(false, 0, 0) and
// button(0, false) ("let go of everything").
using MirrorTouchHandler  = std::function<void(bool down, std::uint16_t x, std::uint16_t y)>;
using MirrorButtonHandler = std::function<void(std::uint8_t code, bool down)>;
void displayMirror_setInputHandlers(MirrorTouchHandler touch, MirrorButtonHandler button);

// HIPI is about to restart (Bootsel mode): tells the viewer (kBye) and
// keeps the mirror going for `ms` instead of a plain sleep, so the viewer
// gets the last picture (the "Entering Bootsel mode" box). Input from the
// viewer is not acted on meanwhile.
void displayMirror_goodbye(std::uint32_t ms);

// Tells the mirror about an off-screen layer a newly connected viewer
// needs: rows x rowBytes from addr, rowStep bytes apart (0: one block).
// assetId != 0 marks a layer filled once from a fixed source (e.g. a
// picture from the SD card): the viewer caches it, so it's only sent the
// first time ever. 0: it changes at run time and is always read back.
// Call once after loading; a later call for the same addr replaces it.
void displayMirror_addLayer(std::uint32_t addr, std::uint32_t rowBytes, std::uint32_t rowStep,
                            std::uint16_t rows, std::uint32_t assetId);
// An asset id for a layer loaded from a file: from its name, size and
// date, plus `salt` (e.g. where in the layer it was drawn). 0 if the
// file doesn't exist.
std::uint32_t displayMirror_fileAssetId(const char* path, std::uint32_t salt);

}  // namespace hipi
