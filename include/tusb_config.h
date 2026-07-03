#pragma once
#include "tusb_option.h"   // för OPT_MODE_DEVICE osv.

// Standard läge: device
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#endif

// Två CDC:er för hp82163 (itf 0 = debug, itf 1 = data)
#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC             2
#endif

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE  256
#endif

#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE  256
#endif

// Stäng av allt vi inte använder (om inte redan definerat)
#ifndef CFG_TUD_HID
#define CFG_TUD_HID             0
#endif
#ifndef CFG_TUD_MSC
#define CFG_TUD_MSC             0
#endif
#ifndef CFG_TUD_MIDI
#define CFG_TUD_MIDI            0
#endif
#ifndef CFG_TUD_VENDOR
#define CFG_TUD_VENDOR          0
#endif
