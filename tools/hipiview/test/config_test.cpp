// The real include/config.hpp (the other tests use stand-ins): a CONFIG.TXT
// from before v1.7.4 names its devices "TFDRIVE", "TFDISPLAY"...; they are
// "DRIVE", "DISPLAY"... now, and the old names are read as the new.
//
//   make config_test && ./config_test   (in tools/hipiview)
#include "config.hpp"
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
}  // namespace

int main() {
    using hipi::Config;
    check(Config::currentDeviceName("TFDRIVE") == "DRIVE", "TFDRIVE is DRIVE");
    check(Config::currentDeviceName("TFDRIVE2") == "DRIVE2", "TFDRIVE2 is DRIVE2");
    check(Config::currentDeviceName("TFDISPLAY") == "DISPLAY", "TFDISPLAY is DISPLAY");
    check(Config::currentDeviceName("TFPLOT") == "PLOTTER", "TFPLOT is PLOTTER");
    check(Config::currentDeviceName("TFPRINT") == "PRINTER", "TFPRINT is PRINTER");
    check(Config::currentDeviceName("TFTERM") == "TERMINAL", "TFTERM is TERMINAL");
    check(Config::currentDeviceName("TFI2C") == "I2C", "TFI2C is I2C");
    check(Config::currentDeviceName("HIPIDRIVE") == "DRIVE", "HIPIDRIVE (a test build) is DRIVE");
    check(Config::currentDeviceName("HIPIPLOT") == "PLOTTER", "HIPIPLOT is PLOTTER");
    check(Config::currentDeviceName("DRIVE") == "DRIVE", "a new name stays");
    check(Config::currentDeviceName("PLOTTER") == "PLOTTER", "PLOTTER stays");
    check(Config::currentDeviceName("PILBOX") == "PILBOX", "PILBOX stays");
    check(Config::currentDeviceName("TFTM070") == "TFTM070", "not a HIPI device: stays");
    check(Config::currentDeviceName("TFDRIVEX") == "TFDRIVEX", "a longer name that isn't ours: stays");
    check(Config::currentDeviceNames("PILBOX,TFDISPLAY,TFDRIVE,TFLEDS,TFTERM") == "PILBOX,DISPLAY,DRIVE,LEDS,TERMINAL",
          "a list of device names");
    check(Config::currentDeviceNames("TFPRINT") == "PRINTER", "a list of one");
    check(Config::currentDeviceNames("").empty(), "an empty list stays empty");
    std::printf(failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
