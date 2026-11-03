/*
 * CTAP2 command handling for the PolKey FIDO2 authenticator (BL616)
 *
 * GetInfo, MakeCredential and GetAssertion. Private keys are generated and
 * used inside the FPGA accelerator; only wrapped blobs are stored on the BL616.
 */
#ifndef CTAP2_H
#define CTAP2_H

#include <stdint.h>
#include <stddef.h>

/* CTAP2 status codes */
#define CTAP2_OK 0x00
#define CTAP2_ERR_INVALID_COMMAND 0x01
#define CTAP2_ERR_INVALID_LENGTH 0x03
#define CTAP2_ERR_INVALID_CBOR 0x12
#define CTAP2_ERR_MISSING_PARAMETER 0x14
#define CTAP2_ERR_CREDENTIAL_EXCLUDED 0x19
#define CTAP2_ERR_PROCESSING 0x21
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26
#define CTAP2_ERR_KEY_STORE_FULL 0x28
#define CTAP2_ERR_UNSUPPORTED_OPTION 0x2B
#define CTAP2_ERR_NO_CREDENTIALS 0x2E
#define CTAP2_ERR_KEEPALIVE_CANCEL 0x2D
#define CTAP2_ERR_USER_ACTION_TIMEOUT 0x2F
#define CTAP2_ERR_INVALID_SEQ 0x04

/* Random bytes: ESP-style TRNG mixed with the FPGA TRNG when the accelerator answers */
void ctap2_random(uint8_t *out, size_t len);

/* One-time init: hardware SHA-256 for rpIdHash (call before the scheduler) */
void ctap2_init(void);

/* Handle one reassembled CTAPHID_CBOR message (task context) */
void ctap2_handle(uint32_t channel, const uint8_t *data, size_t len);

/* Handle one reassembled CTAPHID_MSG (U2F APDU) message */
void ctap2_u2f_handle(uint32_t channel, const uint8_t *data, size_t len);

#endif
