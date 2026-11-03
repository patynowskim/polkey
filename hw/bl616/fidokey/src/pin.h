/*
 * CTAP2 ClientPIN (PIN/UV auth protocols 1 and 2) for the PolKey authenticator
 *
 * The authenticator key-agreement key lives in the FPGA (wrapped blob, ECDH on the accelerator);
 * AES-256-CBC, HMAC-SHA256 and HKDF run in software on the BL616. The PIN is stored as a salted
 * verifier in the metadata sector.
 */
#ifndef PIN_H
#define PIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CTAP2_ERR_NOT_ALLOWED 0x30
#define CTAP2_ERR_PIN_INVALID 0x31
#define CTAP2_ERR_PIN_BLOCKED 0x32
#define CTAP2_ERR_PIN_AUTH_INVALID 0x33
#define CTAP2_ERR_PIN_AUTH_BLOCKED 0x34
#define CTAP2_ERR_PIN_NOT_SET 0x35
#define CTAP2_ERR_PUAT_REQUIRED 0x36
#define CTAP2_ERR_PIN_POLICY_VIOLATION 0x37
#define CTAP2_ERR_INVALID_PARAMETER 0x02
#define CTAP2_ERR_UNAUTHORIZED_PERMISSION 0x40

#define PIN_PERM_MC 0x01
#define PIN_PERM_GA 0x02
#define PIN_PERM_CM 0x04
#define PIN_PERM_BE 0x08
#define PIN_PERM_LBW 0x10
#define PIN_PERM_ACFG 0x20

typedef struct {
    uint8_t protocol;
    uint8_t hmacKey[32];
    uint8_t aesKey[32];
} pin_secret_t;

void pin_init(void);              /* after store_init(): loads the PIN state */
void pin_factory_reset(void);     /* forget PIN, token and key agreement (store already wiped) */
bool pin_is_set(void);

/* authenticatorClientPIN: data = request CBOR (after the command byte). On success *outLen bytes of response CBOR
   are written to out. Returns a CTAP2 status code. */
uint8_t pin_client_pin(const uint8_t *data, size_t len, uint8_t *out, size_t outCap, size_t *outLen);

/* Verify pinUvAuthParam from MakeCredential/GetAssertion (HMAC(token, msg)). On success *uvOut = true. */
uint8_t pin_verify_token_auth(uint8_t protocol, const uint8_t *msg, size_t msgLen,
                              const uint8_t *auth, size_t authLen, uint8_t perm,
                              const uint8_t rpIdHash[32]);
void pin_token_consume(void);     /* single-use after a successful MakeCredential/GetAssertion */

/* Shared secret with a platform key (X||Y) for hmac-secret etc. */
bool pin_shared_secret(uint8_t protocol, const uint8_t peerXY[64], pin_secret_t *out);
bool pin_secret_auth_ok(const pin_secret_t *s, const uint8_t *msg, size_t msgLen, const uint8_t *auth, size_t authLen);
/* AES-CBC with the shared secret; protocol 2 uses a random IV prepended to the ciphertext.
   encrypt: out gets len (+16 for protocol 2) bytes. decrypt: in carries the IV for protocol 2. */
size_t pin_encrypt(const pin_secret_t *s, const uint8_t *in, size_t len, uint8_t *out);
size_t pin_decrypt(const pin_secret_t *s, const uint8_t *in, size_t len, uint8_t *out); /* returns plaintext length, 0 on error */

#endif
