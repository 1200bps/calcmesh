// USB descriptors: the SDK's stdio layout with a second CDC port added.
//
//   interfaces 0-1  CDC "calcmesh console"  stdio (printf, console commands)
//   interface  2    reset                   picotool reboot -u
//   interfaces 3-4  CDC "calcmesh node"     raw bridge to the node (desktop mode)
//   interfaces 5-6  CDC "calcmesh screen"   framebuffer mirror (screenport.h)
//
// The reset interface stays at 2 because the SDK's Microsoft OS 2.0
// descriptor (reset_interface.c) hardcodes that number. On Linux the ports are
// /dev/serial/by-id/usb-calcmesh_*-if00 (console), ...-if03 (node) and
// ...-if05 (screen).

#include "tusb.h"

#include "pico/unique_id.h"
#include "pico/usb_reset_interface.h"

#define USBD_VID 0x2E8A // Raspberry Pi
#if PICO_RP2040
#define USBD_PID 0x000a // Pico SDK CDC, RP2040
#else
#define USBD_PID 0x0009 // Pico SDK CDC
#endif

enum {
    ITF_CONSOLE,
    ITF_CONSOLE_DATA,
    ITF_RESET,
    ITF_NODE,
    ITF_NODE_DATA,
    ITF_SCREEN,
    ITF_SCREEN_DATA,
    ITF_COUNT,
};

enum {
    STR_LANG,
    STR_MANUF,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CONSOLE,
    STR_RESET,
    STR_NODE,
    STR_SCREEN,
    STR_COUNT,
};

#define EP_CONSOLE_NOTIF 0x81
#define EP_CONSOLE_OUT 0x02
#define EP_CONSOLE_IN 0x82
#define EP_NODE_NOTIF 0x83
#define EP_NODE_OUT 0x04
#define EP_NODE_IN 0x84
#define EP_SCREEN_NOTIF 0x85
#define EP_SCREEN_OUT 0x06
#define EP_SCREEN_IN 0x86

#define RESET_DESC_LEN 9
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_CDC_DESC_LEN + RESET_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    // 2.1 so Windows asks for the MS OS 2.0 descriptor (driverless reset).
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t desc_config[CONFIG_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_CONSOLE, STR_CONSOLE, EP_CONSOLE_NOTIF, 8, EP_CONSOLE_OUT,
                       EP_CONSOLE_IN, 64),
    // Vendor interface, no endpoints; the SDK's reset driver claims it.
    9, TUSB_DESC_INTERFACE, ITF_RESET, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC,
    RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, STR_RESET,
    TUD_CDC_DESCRIPTOR(ITF_NODE, STR_NODE, EP_NODE_NOTIF, 8, EP_NODE_OUT, EP_NODE_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_SCREEN, STR_SCREEN, EP_SCREEN_NOTIF, 8, EP_SCREEN_OUT,
                       EP_SCREEN_IN, 64),
};

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const strings[STR_COUNT] = {
    [STR_MANUF] = "calcmesh",
    [STR_PRODUCT] = "Calcmesh",
    [STR_SERIAL] = serial_str,
    [STR_CONSOLE] = "calcmesh console",
    [STR_RESET] = "Reset",
    [STR_NODE] = "calcmesh node",
    [STR_SCREEN] = "calcmesh screen",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_config;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t buf[32];

    if (!serial_str[0])
        pico_get_unique_board_id_string(serial_str, sizeof serial_str);

    uint8_t len;
    if (index == STR_LANG) {
        buf[1] = 0x0409; // English
        len = 1;
    } else {
        if (index >= STR_COUNT || !strings[index])
            return NULL;
        const char *s = strings[index];
        for (len = 0; len < 31 && s[len]; len++)
            buf[1 + len] = (uint8_t)s[len];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return buf;
}
