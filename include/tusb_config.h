#pragma once
#include "tusb_option.h"   // for OPT_MODE_DEVICE etc.
#include "hipi_features.h" // HIPI_MIRROR_PORT

// Default mode: device
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#endif

// Four CDC interfaces for hipi (0 = debug, 1 = data, 2 = terminal,
// 3 = display mirror, see display_mirror.h)
#ifndef CFG_TUD_CDC
#if HIPI_MIRROR_PORT
#define CFG_TUD_CDC             4
#else
#define CFG_TUD_CDC             3
#endif
#endif

// RX FIFO >= CFG_TUD_CDC_EP_BUFSIZE: TinyUSB only arms a port's receive
// endpoint when the RX FIFO has room for a whole EP buffer.
#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE  1024
#endif

#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE  1024
#endif

// CFG_TUD_CDC_EP_BUFSIZE is left at TinyUSB's default: 64 at full speed,
// one USB packet per transfer. Tried 512 (several packets per transfer)
// for the display mirror's throughput: on the RP2350 the stream then lost
// and repeated bytes at transfer boundaries, now and then. Two rules if
// it's ever changed: it must not exceed CFG_TUD_CDC_RX_BUFSIZE (TinyUSB
// only arms a port's receive endpoint when the RX FIFO has room for a
// whole EP buffer -- else no CDC port can receive at all), and the mirror
// stream must be checked for damage (hipiview counts CRC errors).
#if defined(CFG_TUD_CDC_EP_BUFSIZE) && CFG_TUD_CDC_EP_BUFSIZE > CFG_TUD_CDC_RX_BUFSIZE
#error "CFG_TUD_CDC_EP_BUFSIZE must not exceed CFG_TUD_CDC_RX_BUFSIZE"
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
