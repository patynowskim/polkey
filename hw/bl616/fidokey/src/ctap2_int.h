/* Helpers shared between ctap2.c, ctap2_mgmt.c and u2f.c */
#ifndef CTAP2_INT_H
#define CTAP2_INT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cbor.h"
#include "ctaphid.h"

#define CTAP2_MAX_RESP CTAPHID_MAX_MSG

extern uint8_t ctap2_resp[CTAP2_MAX_RESP]; /* byte 0 = status, CBOR from byte 1 */

void ctap2_send_status(uint32_t channel, uint8_t status);
void ctap2_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void ctap2_put_cose_key(cbor_enc_t *e, int alg, const uint8_t *pub, size_t pubLen);

/* User presence through the FPGA board button S2 (ctap2.c). */
#define UP_OK 0
#define UP_TIMEOUT 1
#define UP_CANCELLED 2
/* Waits for a touch, sending CTAPHID keepalives (UPNEEDED). A press from before the call is discarded. Returns UP_*.
   Returns UP_OK at once when user presence is switched off in the settings or the bitstream has no button unit. */
int ctap2_wait_up(uint32_t channel, uint32_t timeout_ms);
/* Non-blocking variant for U2F: true if S2 was pressed within the last 3 s (consumes the press); otherwise optionally asks for a touch with the LEDs */
bool ctap2_poll_up(bool prompt);
void ctap2_ux_housekeeping(void);            /* turns the touch prompt off after a while (call from the idle loop) */
bool ctap2_up_required(void);                /* configured AND the hardware can ask */

/* Attestation identity (u2f.c): wrapped key blob + certificate; *provisioned = a real batch identity is installed */
bool ctap2_attestation(uint8_t blob[64], const uint8_t **cert, size_t *certLen, bool *provisioned);
bool ctap2_att_provision(const uint8_t priv[32], const uint8_t *cert, size_t certLen, const char **err);

size_t ctap2_der_ecdsa(const uint8_t r[32], const uint8_t s[32], uint8_t *out, size_t cap);

/* authenticatorCredentialManagement (0x0A) / authenticatorLargeBlobs (0x0C), in ctap2_mgmt.c */
void ctap2_credential_management(uint32_t channel, const uint8_t *data, size_t len);
void ctap2_large_blobs(uint32_t channel, const uint8_t *data, size_t len);
void ctap2_mgmt_reset(void);

#endif
