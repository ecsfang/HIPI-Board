#ifndef __HIPI_H__
#define __HIPI_H__

// Shared declarations for hipi.cpp's device-enumeration logic -- used by
// both hipi_test()'s boot-time self-check log and boardui.cpp's on-demand
// "Devices" list dialog (bottom-left corner tap), so the two don't
// duplicate the same AAD/SAI/SDI-driven enumeration.

#include <cstdint>
#include <vector>

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

#endif//__HIPI_H__