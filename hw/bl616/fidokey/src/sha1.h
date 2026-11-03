/* Software SHA-1 / HMAC-SHA1 (TOTP, RFC 6238) */
#ifndef SHA1_H
#define SHA1_H
#include <stdint.h>
#include <stddef.h>

void hmac_sha1(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t len, uint8_t out[20]);

#endif
