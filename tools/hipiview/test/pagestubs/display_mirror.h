// Host stand-in (page_chrome_test): no viewer to tell about layers
#pragma once
#include <cstdint>
namespace hipi {
inline void displayMirror_addLayer(std::uint32_t, std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t) {}
}
