/* Software AES-256-GCM, 12-byte IV, 16-byte tag */
#ifndef GCM_H
#define GCM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void aes256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t aadLen,
                        const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
/* false when the tag does not verify (out is zeroed) */
bool aes256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t aadLen,
                        const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out);

#endif
