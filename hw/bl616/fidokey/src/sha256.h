/* Software SHA-256 (the SDK's bflb_sha256_* hardware driver breaks boot behind the Sipeed bootloader) */
#ifndef SHA256_H
#define SHA256_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    uint32_t fill;
} sha256_ctx_t;

void sha256_begin(sha256_ctx_t *c);
void sha256_feed(sha256_ctx_t *c, const uint8_t *d, size_t n);
void sha256_end(sha256_ctx_t *c, uint8_t out[32]);


/* HMAC-SHA256 over up to two concatenated chunks (data2 may be NULL) */
void hmac_sha256(const uint8_t *key, size_t keyLen, const uint8_t *d1, size_t n1, const uint8_t *d2, size_t n2, uint8_t out[32]);
/* PBKDF2-HMAC-SHA256 with precomputed pads (about 4 compressions per iteration) */
void pbkdf2_sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt, size_t saltLen,
                   uint32_t iterations, uint8_t *out, size_t outLen);

/* HKDF-SHA256 (RFC 5869) with a 32-byte output */
void hkdf_sha256_32(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen, const char *info, uint8_t out[32]);

#endif
