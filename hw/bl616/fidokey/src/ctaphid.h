/*
 * CTAPHID transport layer for the PolKey FIDO2 authenticator (BL616)
 *
 * Framing (FIDO CTAPHID, 64-byte HID reports):
 *   INIT packet: CID(4) | CMD(1, bit7=1) | BCNTH | BCNTL | DATA(57)
 *   CONT packet: CID(4) | SEQ(1, bit7=0) | DATA(59)
 * Messages are reassembled in the OUT-endpoint callback and processed by a
 * FreeRTOS task; responses are streamed back one 64-byte packet per IN
 * transfer with the IN-complete callback chaining the next packet.
 */
#ifndef CTAPHID_H
#define CTAPHID_H

#include <stdint.h>
#include <stdbool.h>

#define CTAPHID_PING      0x81
#define CTAPHID_MSG       0x83
#define CTAPHID_INIT      0x86
#define CTAPHID_WINK      0x88
#define CTAPHID_CBOR      0x90
#define CTAPHID_CANCEL    0x91
#define CTAPHID_ERROR     0xBF
#define CTAPHID_KEEPALIVE 0xBB

/* CTAPHID error codes */
#define CTAPHID_ERR_INVALID_CMD      0x01
#define CTAPHID_ERR_INVALID_PAR      0x02
#define CTAPHID_ERR_INVALID_LEN      0x03
#define CTAPHID_ERR_INVALID_SEQ      0x04
#define CTAPHID_ERR_MSG_TIMEOUT      0x05
#define CTAPHID_ERR_CHANNEL_BUSY     0x06
#define CTAPHID_ERR_INVALID_CHANNEL  0x0B

/* Keepalive status */
#define CTAPHID_STATUS_PROCESSING 0x01
#define CTAPHID_STATUS_UPNEEDED   0x02

#define CTAPHID_MAX_MSG 7609
#define CTAPHID_KEM   0xC3 /* vendor command 0x43: ML-KEM keygen / encaps / decaps on the FPGA (needs the vault unlocked) */

/* Add the HID interface + endpoints to the USB device (call before usbd_initialize) */
void ctaphid_usb_init(uint8_t busid);

/* Arm the first OUT read (call on USBD_EVENT_CONFIGURED) */
void ctaphid_start(void);

/* FreeRTOS task: processes reassembled messages */
void ctaphid_task(void *param);

/* Send a CTAPHID response (any context; guarded by critical section) */
/* true once the host sent CTAPHID_CANCEL for the transaction in progress */
bool ctaphid_cancel_requested(void);

void ctaphid_send(uint32_t channel, uint8_t cmd, const uint8_t *data, uint16_t len);

/* Called from the OUT-endpoint callback with one 64-byte report */
void ctaphid_handle_packet(const uint8_t *packet);

#endif
