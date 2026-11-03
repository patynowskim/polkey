#include <string.h>
#include "aes.h"

static uint8_t sbox[256], inv_sbox[256];
static int tables_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

/* S-box from the GF(2^8) inverse + affine transform (no 512-byte constant tables) */
static void make_tables(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ xtime(p));                        /* p *= 3 */
        q ^= (uint8_t)(q << 1);                              /* q /= 3 */
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^
                              ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    tables_ready = 1;
}

void aes256_init(aes256_t *a, const uint8_t key[32])
{
    if (!tables_ready) make_tables();
    uint8_t w[240];
    memcpy(w, key, 32);
    uint8_t rcon = 1;
    for (int i = 32; i < 240; i += 4) {
        uint8_t t[4];
        memcpy(t, w + i - 4, 4);
        if (i % 32 == 0) {
            uint8_t x = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[x];
            rcon = xtime(rcon);
        } else if (i % 32 == 16) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) w[i + j] = (uint8_t)(w[i - 32 + j] ^ t[j]);
    }
    memcpy(a->rk, w, 240);
    memset(w, 0, sizeof(w));
}

static void add_round_key(uint8_t s[16], const uint8_t k[16])
{
    for (int i = 0; i < 16; i++) s[i] ^= k[i];
}

void aes256_encrypt_block(const aes256_t *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk[0]);
    for (int r = 1; r <= 14; r++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        /* ShiftRows (column-major state) */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[c * 4 + row] = s[((c + row) & 3) * 4 + row];
        if (r != 14) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                s[c * 4 + 0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                s[c * 4 + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                s[c * 4 + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                s[c * 4 + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        } else {
            memcpy(s, t, 16);
        }
        add_round_key(s, a->rk[r]);
    }
    memcpy(out, s, 16);
}

void aes256_decrypt_block(const aes256_t *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk[14]);
    for (int r = 13; r >= 0; r--) {
        /* InvShiftRows */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[((c + row) & 3) * 4 + row] = s[c * 4 + row];
        for (int i = 0; i < 16; i++) s[i] = inv_sbox[t[i]];
        add_round_key(s, a->rk[r]);
        if (r != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                s[c * 4 + 0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                s[c * 4 + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                s[c * 4 + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                s[c * 4 + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}

void aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
{
    aes256_t a;
    aes256_init(&a, key);
    uint8_t prev[16], blk[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(in[o + i] ^ prev[i]);
        aes256_encrypt_block(&a, blk, out + o);
        memcpy(prev, out + o, 16);
    }
    memset(&a, 0, sizeof(a));
}

void aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
{
    aes256_t a;
    aes256_init(&a, key);
    uint8_t prev[16], cur[16], blk[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        memcpy(cur, in + o, 16);
        aes256_decrypt_block(&a, cur, blk);
        for (int i = 0; i < 16; i++) out[o + i] = (uint8_t)(blk[i] ^ prev[i]);
        memcpy(prev, cur, 16);
    }
    memset(&a, 0, sizeof(a));
}
