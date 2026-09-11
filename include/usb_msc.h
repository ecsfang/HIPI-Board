// usb_msc.h -- USB Mass Storage (MSC) mode switching for the SD card.
//
// The SD card can't safely be shared between this project's own FatFs
// (used by CDrive/CTapeSD to emulate HP-IL disk/cassette devices) and a
// PC's own filesystem driver at the same time -- both writing
// independently to the same underlying blocks risks corruption. So the
// MSC USB interface (always present in the descriptor -- see
// my_descriptors.c) is only actually readable/writable once explicitly
// switched into "Connect to PC" mode via these functions.
//
// While active: SDOK (the same global CDrive::check() already consults
// before ever touching FatFs -- see drive.cpp) is set false, so HP-IL
// drive/cassette access reports its own normal "no media" condition,
// exactly as if the card had been physically removed -- no special
// awareness of USB MSC needed anywhere else in the HP-IL device code.
// config.hpp's own save() additionally checks usbMscModeActive()
// directly and skips writing while active (settings changes made from
// the menu during this window are simply not persisted, rather than
// racing FatFs against the PC's own filesystem driver) -- everything
// else keeps working completely normally, since nothing else in this
// project touches the SD card at all.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// True while in "Connect to PC" mode (FatFs unmounted, SD card exposed
// raw via MSC).
bool usbMscModeActive();

// Switches into "Connect to PC" mode: closes any currently-open HP-IL
// drive first (so nothing is left with a stale, about-to-be-invalidated
// file handle), unmounts FatFs, and lets the MSC callbacks start serving
// real blocks. Returns false (mode unchanged) if the unmount itself
// fails, e.g. no SD card actually present.
bool enterUsbMscMode();

// Switches back to normal mode: re-mounts FatFs so CDrive/CTapeSD can
// resume. Safe to call even if not currently in MSC mode (no-op then).
void exitUsbMscMode();

#ifdef __cplusplus
}
#endif
