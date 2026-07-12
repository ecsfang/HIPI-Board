// ui_buttons.hpp
#pragma once
#include <cstdint>

namespace hp82163 {

enum class Button : std::uint8_t { None, Shift, Ok, Up, Down, X };

struct ButtonRect { Button id; std::uint16_t x0, y0, x1, y1; };

// Koordinaterna kommer fran layouten i hp41c_buttons_480.svg (0.75x-skalad),
// forskjutna med knapp-bitmapens x0 = screen_width - width = 800 - 120 = 680.
inline constexpr ButtonRect kButtonRects[] = {
    { Button::Shift, 703, 50,  778, 106 },
    { Button::Ok,    703, 131, 778, 187 },
    { Button::Up,    703, 212, 778, 268 },
    { Button::Down,  703, 293, 778, 349 },
    { Button::X,     703, 374, 778, 430 },
};

inline Button hitTestButton(std::uint16_t x, std::uint16_t y) {
    for (const auto& r : kButtonRects) {
        if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) return r.id;
    }
    return Button::None;
}

}  // namespace hp82163