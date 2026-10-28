// hwio_stub.cpp -- see hwio_stub.h
#include "hwio_stub.h"
#include <algorithm>

namespace hwstub {

bool ledsTaken = false, pixelsTaken = false;
bool led[5] = {};
std::vector<Rgb> pixels(32, Rgb{ 0, 0, 0 });
std::vector<Rgb> buffer(32, Rgb{ 0, 0, 0 });
int pixelCount = 5;
int pixelOrder = 0;
int hpilLeds = 2;
bool plotterLed = true;
bool ledsFitted = true, pixelsFitted = true;
int scans = 0;
bool clockAvailable = false;
int clockSets = 0;
std::vector<hipi::I2cFound> bus;

}  // namespace hwstub

namespace hipi {
namespace hwio {

using namespace hwstub;

void takeLeds() {
    ledsTaken = true;
    std::fill(std::begin(led), std::end(led), false);
}
void releaseLeds() {
    ledsTaken = false;
    std::fill(std::begin(led), std::end(led), false);
}
void takePixels() {
    pixelsTaken = true;
    std::fill(buffer.begin(), buffer.end(), Rgb{ 0, 0, 0 });
    pixels = buffer;
}
void releasePixels() {
    pixelsTaken = false;
    std::fill(buffer.begin(), buffer.end(), Rgb{ 0, 0, 0 });
    pixels = buffer;
}
float ledLevel(int index) { return index >= 0 && index < 5 && led[index] ? 100.0f : 0.0f; }
void pixelShown(int index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    r = g = b = 0;
    if (index < 0 || index >= static_cast<int>(pixels.size())) return;
    const Rgb& c = pixels[static_cast<std::size_t>(index)];
    r = c.r; g = c.g; b = c.b;
}

void setLed(int index, bool on) { if (index >= 0 && index < 5) led[index] = on; }

void setPixel(int index, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (index >= 0 && index < static_cast<int>(buffer.size())) buffer[static_cast<std::size_t>(index)] = Rgb{ r, g, b };
}

void showPixels() {
    for (int i = hwstub::pixelCount; i < static_cast<int>(buffer.size()); ++i) buffer[static_cast<std::size_t>(i)] = Rgb{ 0, 0, 0 };
    pixels = buffer;
}

int pixelCount() { return hwstub::pixelCount; }
void setPixelCount(int n) { hwstub::pixelCount = n; }
int pixelOrder() { return hwstub::pixelOrder; }
void setPixelOrder(int order) { hwstub::pixelOrder = order; }
int hpilLeds() { return hwstub::hpilLeds; }
void setHpilLeds(int mode) { hwstub::hpilLeds = mode; }
bool plotterLed() { return hwstub::plotterLed; }
void setPlotterLed(bool on) { hwstub::plotterLed = on; }
bool ledsFitted() { return hwstub::ledsFitted; }
void setLedsFitted(bool f) { hwstub::ledsFitted = f; }
bool pixelsFitted() { return hwstub::pixelsFitted; }
void setPixelsFitted(bool f) { hwstub::pixelsFitted = f; }

std::string loadI2cChips() { return "27 descriptions  (built in + i2c/weather.txt)"; }

bool canSetClock() { return hwstub::clockAvailable; }
bool setClockFromPc(std::string& why) {
    if (!hwstub::clockAvailable) { why = "hipiview isn't connected"; return false; }
    ++hwstub::clockSets;
    return true;
}

int scanI2c(I2cFound* out, int max, bool found[128]) {
    ++scans;
    for (int a = 0; a < 128; ++a) found[a] = false;
    int n = 0;
    for (const I2cFound& f : bus) {
        if (f.state != I2cState::Missing) found[f.addr] = true;
        if (n < max) out[n++] = f;
    }
    return n;
}

}  // namespace hwio
}  // namespace hipi
