#include <string.h>
#include "sha1.h"

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t buf[64];
    uint32_t fill;
} sha1_ctx_t;

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(sha1_ctx_t *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void begin(sha1_ctx_t *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->total = 0;
    c->fill = 0;
}

static void feed(sha1_ctx_t *c, const uint8_t *d, size_t n)
{
    c->total += n;
    while (n) {
        uint32_t k = 64 - c->fill;
        if (k > n) k = (uint32_t)n;
        memcpy(c->buf + c->fill, d, k);
        c->fill += k;
        d += k;
        n -= k;
        if (c->fill == 64) {
            block(c, c->buf);
            c->fill = 0;
        }
    }
}

static void end(sha1_ctx_t *c, uint8_t out[20])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    feed(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) feed(c, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    feed(c, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void hmac_sha1(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t len, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, pad[64], inner[20];
    sha1_ctx_t c;
    if (keyLen > 64) {
        begin(&c);
        feed(&c, key, keyLen);
        end(&c, k);
    } else if (keyLen) {
        memcpy(k, key, keyLen);
    }
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    begin(&c);
    feed(&c, pad, 64);
    feed(&c, data, len);
    end(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5C);
    begin(&c);
    feed(&c, pad, 64);
    feed(&c, inner, 20);
    end(&c, out);
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}
