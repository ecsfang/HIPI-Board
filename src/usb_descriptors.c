// usb_descriptors.c — Device/configuration/string descriptors for dual CDC.
// TinyUSB calls these callbacks during USB enumeration.

#include "tusb.h"
#include <string.h>

// ── Device descriptor ─────────────────────────────────────────────────────────
// bDeviceClass = 0xEF / 0x02 / 0x01 (Misc / IAD) is required when multiple
// CDC interfaces are present.

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,   // Raspberry Pi
    .idProduct          = 0x000A,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&desc_device;
}

// ── Configuration descriptor ──────────────────────────────────────────────────

enum {
    ITF_CDC0_COMM = 0,
    ITF_CDC0_DATA,
    ITF_CDC1_COMM,
    ITF_CDC1_DATA,
    ITF_TOTAL
};

#define EP_CDC0_NOTIFY  0x81
#define EP_CDC0_OUT     0x02
#define EP_CDC0_IN      0x82
#define EP_CDC1_NOTIFY  0x83
#define EP_CDC1_OUT     0x04
#define EP_CDC1_IN      0x84

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // CDC0 — debug console (printf)
    TUD_CDC_DESCRIPTOR(ITF_CDC0_COMM, 4, EP_CDC0_NOTIFY, 8,
                       EP_CDC0_OUT, EP_CDC0_IN, 64),

    // CDC1 — data port
    TUD_CDC_DESCRIPTOR(ITF_CDC1_COMM, 5, EP_CDC1_NOTIFY, 8,
                       EP_CDC1_OUT, EP_CDC1_IN, 64),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ── String descriptors ────────────────────────────────────────────────────────

static char const* string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   // 0: English
    "HIPI Board",                    // 1: Manufacturer
    "HIPI Dual CDC",                 // 2: Product
    "HIPI-0001",                     // 3: Serial number
    "HIPI CDC0 - Console",           // 4: CDC0 name
    "HIPI CDC1 - Data",              // 5: CDC1 name
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;
        const char* str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}