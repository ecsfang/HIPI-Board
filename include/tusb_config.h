#pragma once
#include "tusb_option.h"   // for OPT_MODE_DEVICE etc.

// Default mode: device
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#endif

// Three CDC interfaces for hipi (0 = debug, 1 = data, 2 = terminal)
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
// Must be a multiple of the 512-byte sector size. 4096 lets TinyUSB move
// 8 sectors per READ10/WRITE10 callback, i.e. one multi-block SPI transfer
// instead of 8 single-block ones -- much faster when the host mounts or
// scans the card. Costs 4 kB of RAM.
#define CFG_TUD_MSC_EP_BUFSIZE  4096
#endif

// Disable everything we don't use (unless already defined)
#ifndef CFG_TUD_HID
#define CFG_TUD_HID             0
#endif
#ifndef CFG_TUD_MIDI
#define CFG_TUD_MIDI            0
#endif
#ifndef CFG_TUD_VENDOR
#define CFG_TUD_VENDOR          0
#endif
