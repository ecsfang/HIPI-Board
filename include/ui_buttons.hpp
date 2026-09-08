// ui_buttons.hpp
#pragma once
#include <cstdint>
#include "display_config.h"
#include "usb_serial.h"

namespace hipi {

enum class Button : std::uint8_t { None, Shift, Ok, Up, Down, X };

struct ButtonRect { Button id; std::uint16_t x0, y0, x1, y1; };

// Coordinates are LOCAL to the button-strip bitmap's own top-left corner,
// at the ORIGINAL 120x480 artwork resolution (resources/buttons.bmp, as
// designed for the 5" board's panel) -- NOT absolute screen coordinates.
// hitTestButton() below maps an incoming absolute touch into this local,
// original-resolution space using the strip's CURRENT actual screen
// position (buttonStripScreenX0, tracked live in boardui.cpp -- shifts
// with panel width) and its current actual height (buttonStripHeight --
// scaled at load time for panels taller than 480px, see
// boardui_loadButtonStrip()'s own comment). Kept this way, rather than
// baking absolute/pre-scaled coordinates in here directly, specifically
// so it stays correct across different panel widths/heights without
// needing hand-recomputed constants for each -- only kSourceHeight below
// needs to match the ORIGINAL artwork if that's ever regenerated at a
// different design resolution.
//
// Local X (24-96) = the original 704-776 minus the OLD strip's own
// screen offset (680 = 800 - 120 on the 5" board's panel) -- width
// itself was never scaled (only height was, see the loader), so these
// two numbers are still directly correct as-is, just no longer
// expressed as absolute screen X.
constexpr std::uint16_t kSourceHeight = 480;  // original buttons.bmp design height

inline constexpr ButtonRect kButtonRects[] = {
    { Button::Shift, 24, 16,  96, 72  },
    { Button::Ok,    24, 100, 96, 156 },
    { Button::Up,    24, 184, 96, 240 },
    { Button::Down,  24, 268, 96, 324 },
    { Button::X,     24, 352, 96, 408 },
};

inline Button hitTestButton(std::uint16_t x, std::uint16_t y,
                            std::uint16_t stripScreenX0, std::uint16_t stripHeight) {
    if (x < stripScreenX0) return Button::None;
    const std::uint16_t localX = static_cast<std::uint16_t>(x - stripScreenX0);
    // Scale the incoming Y down to the ORIGINAL design resolution before
    // comparing against kButtonRects' own original-resolution Y ranges --
    // the inverse of the scaling the loader applied when it stretched
    // the bitmap up to stripHeight.
    const std::uint16_t localY = (stripHeight == kSourceHeight)
        ? y
        : static_cast<std::uint16_t>(
              (static_cast<std::uint32_t>(y) * kSourceHeight) / stripHeight);
    for (const auto& r : kButtonRects) {
        if (localX >= r.x0 && localX <= r.x1 && localY >= r.y0 && localY <= r.y1) return r.id;
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
inline void redrawButtonRegion(DisplayDriver* display,
                               const std::uint16_t* stripPixels,
                               std::uint16_t stripWidth, std::uint16_t stripHeight,
                               std::uint16_t stripScreenX0, std::uint16_t stripScreenY0,
                               Button b, std::int16_t dx, std::int16_t dy,
                               std::int16_t margin = 0) {
    if (!stripPixels) return;
    for (const auto& r : kButtonRects) {
        if (r.id != b) continue;

        // r.x0/x1/y0/y1 are local-to-strip, at the ORIGINAL (kSourceHeight)
        // design resolution -- X is used directly (width was never
        // scaled, so it's already correct for stripPixels as cached), Y
        // needs scaling up to stripHeight to match how the cache was
        // actually stretched at load time (see boardui_loadButtonStrip()).
        const auto scaleY = [&](std::uint16_t v) -> int {
            return (stripHeight == kSourceHeight)
                ? static_cast<int>(v)
                : static_cast<int>((static_cast<std::uint32_t>(v) * stripHeight) / kSourceHeight);
        };
        const int sy0 = scaleY(r.y0);
        const int sy1 = scaleY(r.y1);

        const int w = (r.x1 - r.x0 + 1) + 2 * margin;
        const int h = (sy1 - sy0 + 1) + 2 * margin;
        const int localX0 = static_cast<int>(r.x0) - margin;
        const int localY0 = sy0 - margin;
        if (localX0 < 0 || localY0 < 0 ||
            localX0 + w > stripWidth || localY0 + h > stripHeight) {
            return;  // expanded rect doesn't fit inside the cached bitmap -- ignore
        }

        // ONE call to drawBitmap565Cropped(), not a per-row loop calling
        // drawBitmap565() repeatedly -- a worthwhile optimization
        // regardless (avoids redundant gfxMode()/ICR round-trips per
        // row), but NOT actually the cause of the wrong-position bug this
        // was first suspected to explain. The real cause, confirmed on
        // real hardware: showButtonStrip() narrows the active window to
        // exclude the strip's own screen area once it finishes drawing,
        // and nothing here was resetting it back to the full panel before
        // drawing INTO that excluded area again -- see boardui.cpp's own
        // setActiveWindow() calls right before each call into this
        // function for the actual fix. drawBitmap565Cropped() already
        // supports a source stride different from the drawn width, which
        // is exactly what's needed here (stripWidth vs this button's own
        // narrower w) -- one call, letting its own internal row loop
        // handle everything.
        const std::uint16_t* src = stripPixels + static_cast<std::size_t>(localY0) * stripWidth + localX0;
        display->drawBitmap565Cropped(
            static_cast<std::int16_t>(stripScreenX0 + r.x0 - margin + dx),
            static_cast<std::int16_t>(stripScreenY0 + sy0 - margin + dy),
            static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(h),
            stripWidth, src);
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
// Same local-to-strip, original-resolution coordinate scheme as
// kButtonRects above -- see its own comment for the full rationale.
// Local cx (37/83) = original absolute 717/763 minus the OLD strip's
// screen offset (680); cy (452) is still at the original design
// resolution, scaled dynamically in setStatusLed() below.
enum class StatusLed { Usb, Pil };

struct StatusLedSpec { std::uint16_t cx, cy, radius; };

inline constexpr StatusLedSpec kUsbLed = { 37, 452, 10 };
inline constexpr StatusLedSpec kPilLed = { 83, 452, 10 };

inline constexpr std::uint16_t kStatusLedOnColor  = 0x07E0;  // green
inline constexpr std::uint16_t kStatusLedOffColor = 0x0000;  // black

inline void setStatusLed(DisplayDriver* display, StatusLed which, bool on,
                         std::uint16_t stripScreenX0, std::uint16_t stripHeight) {
    const StatusLedSpec& s = (which == StatusLed::Usb) ? kUsbLed : kPilLed;
    const std::uint16_t screenCx = static_cast<std::uint16_t>(stripScreenX0 + s.cx);
    const std::uint16_t screenCy = (stripHeight == kSourceHeight)
        ? s.cy
        : static_cast<std::uint16_t>(
              (static_cast<std::uint32_t>(s.cy) * stripHeight) / kSourceHeight);
    const std::int16_t d = static_cast<std::int16_t>(s.radius * 2);
    display->fillRoundRect(static_cast<std::int16_t>(screenCx - s.radius),
                          static_cast<std::int16_t>(screenCy - s.radius),
                          d, d, s.radius,
                          on ? kStatusLedOnColor : kStatusLedOffColor);
}

}  // namespace hipi