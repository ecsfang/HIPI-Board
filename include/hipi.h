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
    // PILBOX only: devices on the PC side and the first address they got
    // at the last auto-addressing (-1 = not known / not applicable)
    int extDevices = -1;
    int extFirstAddr = -1;
};

// Lists every device in hipi.cpp's own `devices` vector, in loop order,
// with its CURRENT address as given by the controller's own AAU/AAD
// (addr = -1 if it has none: disabled, or not addressed yet), accessory ID
// and enabled state. Read-only: no frames are sent, so it's safe to call
// at any time.
std::vector<DeviceInfo> hipi_enumerateDevices();

// Makes `order` (the same devices, rearranged) the new loop order and
// saves it to CONFIG.TXT (device_order). Addresses aren't changed -- the
// HP-41 reassigns them in the new order at its next auto-addressing.
class CDevice;
void hipi_applyDeviceOrder(const std::vector<CDevice*>& order);

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