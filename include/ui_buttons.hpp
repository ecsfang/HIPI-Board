// ui_buttons.hpp
#pragma once
#include <cstdint>
#include "RA8875.hpp"

namespace hp82163 {

enum class Button : std::uint8_t { None, Shift, Ok, Up, Down, X };

struct ButtonRect { Button id; std::uint16_t x0, y0, x1, y1; };

// Koordinaterna ar extraherade direkt ur den nya buttons.bmp (den svarta
// knapp-konturens pixelgranser), sedan forskjutna med knapp-bitmapens
// x0 = screen_width - width = 800 - 120 = 680. Y-koordinaterna behover ingen
// forskjutning (remsan ritas vid y0=0). Om buttons.bmp byts ut igen, kor om
// samma extrahering istallet for att gissa nya varden for hand.
// Coordinates match resources/buttons.bmp exactly (from the generation
// script -- buttons shifted up from the previous layout to make room for
// the USB/PIL status section at the bottom). X range unchanged.
inline constexpr ButtonRect kButtonRects[] = {
    { Button::Shift, 704, 16,  776, 72  },
    { Button::Ok,    704, 100, 776, 156 },
    { Button::Up,    704, 184, 776, 240 },
    { Button::Down,  704, 268, 776, 324 },
    { Button::X,     704, 352, 776, 408 },
};

inline Button hitTestButton(std::uint16_t x, std::uint16_t y) {
    for (const auto& r : kButtonRects) {
        if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) return r.id;
    }
    return Button::None;
}

// Redraws a single button's sub-rectangle from a cached copy of the full
// button-strip bitmap (see drawBmpRightAligned's outPixels parameter in
// pico_main.cpp), optionally shifted by (dx, dy) pixels -- used for the
// "pressed" visual feedback, and to restore it on release.
//
//   stripPixels          -- cached RGB565 pixels, row-major, stride = stripWidth
//   stripWidth/Height     -- dimensions of that cached bitmap
//   stripScreenX0/Y0      -- where the strip was originally drawn on screen
//                            (so button screen-coordinates can be mapped
//                            back into the cached bitmap's local coordinates)
//   margin                -- expands the redrawn rect by this many pixels on
//                            every side (still sourced from the strip cache,
//                            so the extra pixels are genuine surrounding
//                            background, not guesswork). Use a margin >=
//                            your largest press-shift on the "restore to
//                            normal" calls, so any overflow from a shifted
//                            press -- in any direction -- gets cleaned up.
inline void redrawButtonRegion(RA8875* display,
                               const std::uint16_t* stripPixels,
                               std::uint16_t stripWidth, std::uint16_t stripHeight,
                               std::uint16_t stripScreenX0, std::uint16_t stripScreenY0,
                               Button b, std::int16_t dx, std::int16_t dy,
                               std::int16_t margin = 0) {
    if (!stripPixels) return;
    for (const auto& r : kButtonRects) {
        if (r.id != b) continue;

        const int w = (r.x1 - r.x0 + 1) + 2 * margin;
        const int h = (r.y1 - r.y0 + 1) + 2 * margin;
        const int localX0 = static_cast<int>(r.x0) - margin - static_cast<int>(stripScreenX0);
        const int localY0 = static_cast<int>(r.y0) - margin - static_cast<int>(stripScreenY0);
        if (localX0 < 0 || localY0 < 0 ||
            localX0 + w > stripWidth || localY0 + h > stripHeight) {
            return;  // expanded rect doesn't fit inside the cached bitmap -- ignore
        }

        // Each row is contiguous within itself even though the cached
        // buffer's stride (stripWidth) differs from the button's own
        // width, so we can blit row-by-row straight out of the cache
        // without needing to repack into a temporary buffer.
        for (int row = 0; row < h; ++row) {
            const std::uint16_t* src =
                stripPixels + static_cast<std::size_t>(localY0 + row) * stripWidth + localX0;
            display->drawBitmap565(static_cast<std::int16_t>(r.x0 - margin + dx),
                                  static_cast<std::int16_t>(r.y0 - margin + row + dy),
                                  static_cast<std::uint16_t>(w), 1, src);
        }
        return;
    }
}

// ── Status indicator LEDs (USB / HP-IL) ─────────────────────────────────────
//
// Drawn directly as filled circles (via RA8875::fillRoundRect() with
// radius == half the bounding box, which produces a true circle) rather
// than being part of the cached button-strip bitmap -- so they can be
// toggled from code at any time, independent of touch interaction.
// resources/buttons.bmp already bakes in the "off" (black) state plus a
// dark bezel and the "USB"/"PIL" labels; setStatusLed() only needs to
// redraw the small circle itself, in either black (off) or green (on).
//
// Coordinates are screen coordinates (local button-panel position + the
// strip's own x0 offset, matching kButtonRects), taken directly from the
// generation script that produced resources/buttons.bmp -- see the status
// section layout there if the artwork ever changes.
enum class StatusLed { Usb, Pil };

struct StatusLedSpec { std::uint16_t cx, cy, radius; };

inline constexpr StatusLedSpec kUsbLed = { 717, 452, 10 };
inline constexpr StatusLedSpec kPilLed = { 763, 452, 10 };

inline constexpr std::uint16_t kStatusLedOnColor  = 0x07E0;  // green
inline constexpr std::uint16_t kStatusLedOffColor = 0x0000;  // black

inline void setStatusLed(RA8875* display, StatusLed which, bool on) {
    const StatusLedSpec& s = (which == StatusLed::Usb) ? kUsbLed : kPilLed;
    const std::int16_t d = static_cast<std::int16_t>(s.radius * 2);
    display->fillRoundRect(static_cast<std::int16_t>(s.cx - s.radius),
                          static_cast<std::int16_t>(s.cy - s.radius),
                          d, d, s.radius,
                          on ? kStatusLedOnColor : kStatusLedOffColor);
}

}  // namespace hp82163