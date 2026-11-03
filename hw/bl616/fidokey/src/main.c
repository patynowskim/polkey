/*
 * BL616 PolKey FIDO2 authenticator — Main entry point
 *
 * Composite USB device: DirtyJTAG v2 (vendor bulk) + FIDO HID (CTAPHID)
 */
#include "board.h"
#include "usbd_core.h"
#include "FreeRTOS.h"
#include "task.h"

#include "dirtyjtag.h"
#include "jtag_gpio.h"
#include "ctaphid.h"

/* Provided by usb_descriptors.c */
extern void usb_descriptors_init(uint8_t busid);

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_RESET:
        break;
    case USBD_EVENT_CONFIGURED:
        dirtyjtag_start();
        ctaphid_start();
        break;
    default:
        break;
    }
}

int main(void)
{
    board_init();

    /* Init USB composite device first (same order as the proven PolKey firmware) */
    usb_descriptors_init(0);
    dirtyjtag_init(0);
    ctaphid_usb_init(0);
    usbd_initialize(0, 0x20072000, usbd_event_handler);

    /* Init GPIO for JTAG */
    jtag_gpio_init();

    /* CTAPHID command processor (runs the crypto/storage/link init itself) */
    xTaskCreate(ctaphid_task, "ctaphid", 4096, NULL, 2, NULL);

    vTaskStartScheduler();

    while (1) {}
}
