#include "tusb.h"
#include "device/usbd.h"

// Bosch/serial-style strings. Byt ut 0x2E8A om du har en egen VID/PID.
#define USB_VID   0x2E8A
#define USB_PID   0x000B
#define USB_BCD   0x0200

// ─── Device descriptor ───────────────────────────────────────────────────
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

// ─── Configuration descriptor: 2 × CDC ──────────────────────────────────
#define EPNUM_CDC0_NOTIF   0x81
#define EPNUM_CDC0_OUT     0x02
#define EPNUM_CDC0_IN      0x82
#define EPNUM_CDC1_NOTIF   0x83
#define EPNUM_CDC1_OUT     0x04
#define EPNUM_CDC1_IN      0x84

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0,
        (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN * 2),
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC0_NOTIF, 8,           // CDC0: itf 0+1
                       EPNUM_CDC0_OUT, EPNUM_CDC0_IN, 64),

    TUD_CDC_DESCRIPTOR(2, 5, EPNUM_CDC1_NOTIF, 8,           // CDC1: itf 2+3  ← FIXEN
                       EPNUM_CDC1_OUT, EPNUM_CDC1_IN, 64),
};

// ─── String descriptors ──────────────────────────────────────────────────
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},      // 0: språk (engelska)
    "HP82163",                       // 1: manufacturer
    "HP-IL Bridge",                  // 2: product
    "PICO2-0001",                    // 3: serial
    "HP-IL Debug",                   // 4: CDC0
    "HP-IL Data",                    // 5: CDC1
};

// ─── TinyUSB hooks ───────────────────────────────────────────────────────
uint8_t const * tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint16_t const * tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t buf[32];
    const char *str;
    if (index == 0) { buf[0] = 0x0409; return buf; }
    if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) return NULL;
    str = string_desc_arr[index];
    uint8_t len = strlen(str);
    if (len > 31) len = 31;
    for (uint8_t i = 0; i < len; i++) buf[i+1] = str[i];
    buf[0] = (0x03 << 8) | (2 * len + 2);
    return buf;
}
