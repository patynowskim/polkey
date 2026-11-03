#include <string.h>
#include "gcm.h"
#include "aes.h"

/* GF(2^128) multiply, Z = X * H (bitwise, big-endian bit order as in SP 800-38D) */
static void gmul(const uint8_t x[16], const uint8_t h[16], uint8_t z[16])
{
    uint8_t v[16], r[16] = { 0 };
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7))) {
            for (int j = 0; j < 16; j++) r[j] ^= v[j];
        }
        uint8_t lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(z, r, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t *d, size_t n)
{
    while (n) {
        uint8_t blk[16] = { 0 };
        size_t k = n < 16 ? n : 16;
        memcpy(blk, d, k);
        for (int i = 0; i < 16; i++) y[i] ^= blk[i];
        gmul(y, h, y);
        d += k;
        n -= k;
    }
}

static void inc32(uint8_t cb[16])
{
    for (int i = 15; i >= 12; i--) {
        if (++cb[i]) break;
    }
}

static void gcm_core(const aes256_t *a, const uint8_t iv[12], const uint8_t *aad, size_t aadLen,
                     const uint8_t *in, size_t len, uint8_t *out, bool encrypt, uint8_t tag[16])
{
    uint8_t h[16] = { 0 }, y[16] = { 0 }, j0[16], cb[16], ks[16];
    aes256_encrypt_block(a, h, h);

    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    memcpy(cb, j0, 16);

    ghash_update(y, h, aad, aadLen);
    if (!encrypt) ghash_update(y, h, in, len);

    for (size_t o = 0; o < len; o += 16) {
        inc32(cb);
        aes256_encrypt_block(a, cb, ks);
        size_t k = len - o < 16 ? len - o : 16;
        for (size_t i = 0; i < k; i++) out[o + i] = (uint8_t)(in[o + i] ^ ks[i]);
    }
    if (encrypt) ghash_update(y, h, out, len);

    uint8_t lens[16];
    uint64_t ab = (uint64_t)aadLen * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(ab >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    for (int i = 0; i < 16; i++) y[i] ^= lens[i];
    gmul(y, h, y);

    aes256_encrypt_block(a, j0, ks);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i] ^ ks[i]);
}

void aes256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t aadLen,
                        const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
    aes256_t a;
    aes256_init(&a, key);
    gcm_core(&a, iv, aad, aadLen, in, len, out, true, tag);
    memset(&a, 0, sizeof(a));
}

bool aes256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t aadLen,
                        const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out)
{
    aes256_t a;
    uint8_t t[16];
    aes256_init(&a, key);
    gcm_core(&a, iv, aad, aadLen, in, len, out, false, t);
    memset(&a, 0, sizeof(a));
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= (uint8_t)(t[i] ^ tag[i]);
    if (d) {
        memset(out, 0, len);
        return false;
    }
    return true;
}
