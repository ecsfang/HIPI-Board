#ifndef __HIPI_H__
#define __HIPI_H__

// Shared declarations for hipi.cpp's device-enumeration logic -- used by
// both hipi_test()'s boot-time self-check log and boardui.cpp's on-demand
// "Devices" list dialog (bottom-left corner tap), so the two don't
// duplicate the same AAD/SAI/SDI-driven enumeration.

#include <cstdint>
#include <vector>
#include <functional>

#include "hpil_pio.hpp"
#include "loopback_result.h"

struct DeviceInfo {
    char devName[16];   // dev->name() -- our own local identifier
    char sdiName[32];   // whatever the device's SDI response actually said
    int addr;           // -1 if not addressed (AAD left it at 31, "unaddressed")
    std::uint8_t sai;   // only meaningful if addr >= 0
    bool enabled;
};

// Enumerates every device in hipi.cpp's own `devices` vector by directly
// driving dev->hpil() (never touching the physical PIO loop -- see
// hipi_test()'s own comment on why that's the only thing safe to do
// without knowing what else might be on the bus). This temporarily
// re-addresses every device via an internal AAD/TAD/SAI/SDI sequence, the
// same way a real controller's own start-up handshake would -- calling
// it while a real controller is actively mid-transaction on the physical
// loop risks disrupting that specific exchange, so it's only safe to call
// when nothing else is expected to be talking on the loop at that moment.
std::vector<DeviceInfo> hipi_enumerateDevices();

// Sends values from a fixed test range out over the physical HP-IL loop
// and confirms each one comes back unchanged -- requires an actual
// loopback jumper connecting OUT to IN (see the menu's own "Loopback
// test" confirmation dialog, uidialog.hpp), since with nothing connected
// the very first value simply times out. Moved out of hipi_test() (which
// now only covers the device self-check, and no longer touches the
// physical loop at boot at all) -- this is menu-driven only now, via
// Config > Loopback test.
//
// Stops at the first failure (timeout or wrong value) rather than
// working through the whole range regardless -- confirmed painfully slow
// otherwise with nothing connected (every one of ~1000 values timing out
// in turn, each taking its own 100ms). See LoopbackResult's own comment
// for what tested/errors/failedCmd mean in that case.
//
// onProgress, if given, is called roughly once a second (not on every
// value -- there are far too many of those, and updating the display
// that often would slow the test down for no benefit) with how many
// values have been tested so far, the total the test is attempting, and
// the most recent value sent -- enough for a caller to redraw a percent-
// complete indicator without needing to poll anything itself.
LoopbackResult hipi_loopbackTest(
    HpIlLoop& loop,
    std::function<void(int tested, int total, std::uint32_t lastCmd)> onProgress = nullptr);

void hipi_init(void);
bool hipi_loop(HpIlLoop& loop);
bool hipi_test();

#endif//__HIPI_H__