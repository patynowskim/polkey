/* Software AES-256 (ECB block ops + CBC helpers) for the CTAP2 PIN protocols */
#ifndef AES_H
#define AES_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t rk[15][16];
} aes256_t;

void aes256_init(aes256_t *a, const uint8_t key[32]);
void aes256_encrypt_block(const aes256_t *a, const uint8_t in[16], uint8_t out[16]);
void aes256_decrypt_block(const aes256_t *a, const uint8_t in[16], uint8_t out[16]);

/* CBC without padding; len % 16 == 0; in/out may alias */
void aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out);
void aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out);

#endif
