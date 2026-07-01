#pragma once

// tusb_config.h — TinyUSB configuration for two CDC ACM interfaces
// Place this file in a directory on the compiler include path (e.g. include/).

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_PICO

// Two CDC ACM ports
#define CFG_TUD_CDC             2
#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  256

// Unused device classes
#define CFG_TUD_HID             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0