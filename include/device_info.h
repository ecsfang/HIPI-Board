// device_info.h -- what is known about one of HIPI's devices on the loop
// (hipi_enumerateDevices(), hipi.h), for the HP-IL DEVICES list
// (devicelist.h). Kept apart from hipi.h so the list can be drawn on the
// PC (tools/hipiview) without the HP-IL hardware headers.
#pragma once
#include <cstdint>

struct DeviceInfo {
    char devName[16];   // dev->name() -- our own local identifier
    char sdiName[32];   // the HP device it emulates ("HP7470A", deviceModel()), "" for HIPI's own
    int addr;           // -1 if not addressed (AAD left it at 31, "unaddressed")
    std::uint8_t sai;   // only meaningful if addr >= 0
    bool enabled;
    // PILBOX only: devices on the PC side and the first address they got
    // at the last auto-addressing (-1 = not known / not applicable)
    int extDevices = -1;
    int extFirstAddr = -1;
    // PILBOX only: the link mode to the PC -- "COFF"/"COFI"/"CON" when a PC
    // program is connected, "TDIS" when not; "" for other devices
    char extMode[6] = "";
};
