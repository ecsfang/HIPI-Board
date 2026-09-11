#pragma once
#include "tusb_option.h"   // för OPT_MODE_DEVICE osv.

// Standard läge: device
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#endif

// Två CDC:er för hipi (itf 0 = debug, itf 1 = data)
#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC             3
#endif

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE  256
#endif

#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE  256
#endif

// MSC (USB Mass Storage) -- exposes the SD card as a raw USB drive to
// the PC, alongside the 3 CDC serial interfaces above (see usb_msc.hpp
// for the full story and the mode-switching this requires). Only ONE
// logical unit (the SD card itself) is ever needed.
#ifndef CFG_TUD_MSC
#define CFG_TUD_MSC             1
#endif
#ifndef CFG_TUD_MSC_EP_BUFSIZE
#define CFG_TUD_MSC_EP_BUFSIZE  512  // matches the SD card's own sector size
#endif

// Stäng av allt vi inte använder (om inte redan definerat)
#ifndef CFG_TUD_HID
#define CFG_TUD_HID             0
#endif
#ifndef CFG_TUD_MIDI
#define CFG_TUD_MIDI            0
#endif
#ifndef CFG_TUD_VENDOR
#define CFG_TUD_VENDOR          0
#endif
