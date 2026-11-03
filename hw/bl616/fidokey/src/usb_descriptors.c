/*
 * USB descriptors for PolKey FIDO2: DirtyJTAG + FIDO HID
 *
 * Interface 0: Vendor bulk (DirtyJTAG)   — EP 0x01 OUT, EP 0x82 IN
 * Interface 1: HID (FIDO/CTAPHID)        — EP 0x03 OUT, EP 0x84 IN, 64-byte interrupt
 * (Not CDC-ACM: the BL616 USB controller has one bidirectional FIFO per endpoint number 1..4, and the CDC
 *  notification endpoint would have to share a number with a bulk endpoint, which breaks short OUT packets.)
 */
#include "usbd_core.h"
#include "ctaphid.h"

/* Endpoint addresses */
#define JTAG_OUT_EP  0x01
#define JTAG_IN_EP   0x82
#define HID_OUT_EP   0x03
#define HID_IN_EP    0x84

#define USBD_VID           0x1209
#define USBD_PID           0xC0CA
#define USBD_MAX_POWER     100

/* Vendor interface descriptor length: interface(9) + EP OUT(7) + EP IN(7) = 23 */
#define VENDOR_INTF_DESC_LEN (9 + 7 + 7)
/* HID interface descriptor length: interface(9) + HID(9) + EP OUT(7) + EP IN(7) = 32 */
#define HID_INTF_DESC_LEN (9 + 9 + 7 + 7)

/* Total config descriptor length */
#define USB_CONFIG_SIZE (9 + VENDOR_INTF_DESC_LEN + HID_INTF_DESC_LEN)

/* Number of interfaces: JTAG + FIDO HID */
#define INTF_NUM 2

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0200, 0x01),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interface 0: Vendor bulk (DirtyJTAG) */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x02),
    /* Endpoint OUT 1 */
    USB_ENDPOINT_DESCRIPTOR_INIT(JTAG_OUT_EP, USB_ENDPOINT_TYPE_BULK, 0x0040, 0x00),
    /* Endpoint IN 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(JTAG_IN_EP, USB_ENDPOINT_TYPE_BULK, 0x0040, 0x00),
    /* Interface 1: HID (FIDO/CTAPHID) */
    USB_INTERFACE_DESCRIPTOR_INIT(0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00),
    /* HID descriptor (bcdHID 1.11, 1 report descriptor of length 34) */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x22, 0x00,
    /* Endpoint OUT 3 (interrupt, 64 B, 5 ms) */
    USB_ENDPOINT_DESCRIPTOR_INIT(HID_OUT_EP, USB_ENDPOINT_TYPE_INTERRUPT, 0x0040, 0x05),
    /* Endpoint IN 4 (interrupt, 64 B, 5 ms) */
    USB_ENDPOINT_DESCRIPTOR_INIT(HID_IN_EP, USB_ENDPOINT_TYPE_INTERRUPT, 0x0040, 0x05),
};

static const uint8_t device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01),
};

static const uint8_t other_speed_config_descriptor[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interface 0: Vendor bulk (DirtyJTAG) */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x02),
    USB_ENDPOINT_DESCRIPTOR_INIT(JTAG_OUT_EP, USB_ENDPOINT_TYPE_BULK, 0x0040, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(JTAG_IN_EP, USB_ENDPOINT_TYPE_BULK, 0x0040, 0x00),
    /* Interface 1: HID (FIDO/CTAPHID) */
    USB_INTERFACE_DESCRIPTOR_INIT(0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00),
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x22, 0x00,
    USB_ENDPOINT_DESCRIPTOR_INIT(HID_OUT_EP, USB_ENDPOINT_TYPE_INTERRUPT, 0x0040, 0x05),
    USB_ENDPOINT_DESCRIPTOR_INIT(HID_IN_EP, USB_ENDPOINT_TYPE_INTERRUPT, 0x0040, 0x05),
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },  /* Langid */
    "PolTech",                      /* Manufacturer */
    "PolKey",                       /* Product */
    "0200",                         /* Serial Number */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

static const uint8_t *other_speed_config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return other_speed_config_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
        return NULL;
    }
    return string_descriptors[index];
}

const struct usb_descriptor polkey_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

void usb_descriptors_init(uint8_t busid)
{
    usbd_desc_register(busid, &polkey_descriptor);
}
