#include <string.h>
#include "cbor.h"

static void put(cbor_enc_t *e, uint8_t b)
{
    if (e->off < e->cap) {
        e->buf[e->off++] = b;
    } else {
        e->ok = false;
    }
}

static void put_type_val(cbor_enc_t *e, uint8_t major, uint64_t v)
{
    uint8_t mt = major << 5;

    if (v < 24) {
        put(e, mt | (uint8_t)v);
    } else if (v < 0x100) {
        put(e, mt | 24);
        put(e, (uint8_t)v);
    } else if (v < 0x10000) {
        put(e, mt | 25);
        put(e, (uint8_t)(v >> 8));
        put(e, (uint8_t)v);
    } else if (v < 0x100000000ULL) {
        put(e, mt | 26);
        put(e, (uint8_t)(v >> 24));
        put(e, (uint8_t)(v >> 16));
        put(e, (uint8_t)(v >> 8));
        put(e, (uint8_t)v);
    } else {
        put(e, mt | 27);
        for (int i = 7; i >= 0; i--) {
            put(e, (uint8_t)(v >> (8 * i)));
        }
    }
}

void cbor_init(cbor_enc_t *e, uint8_t *buf, size_t cap)
{
    e->buf = buf;
    e->cap = cap;
    e->off = 0;
    e->ok = true;
}

void cbor_map(cbor_enc_t *e, size_t n)
{
    put_type_val(e, 5, n);
}

void cbor_array(cbor_enc_t *e, size_t n)
{
    put_type_val(e, 4, n);
}

void cbor_uint(cbor_enc_t *e, uint64_t v)
{
    put_type_val(e, 0, v);
}

void cbor_negint(cbor_enc_t *e, int64_t v)
{
    put_type_val(e, 1, (uint64_t)(-1 - v));
}

void cbor_bytes(cbor_enc_t *e, const uint8_t *data, size_t len)
{
    put_type_val(e, 2, len);
    for (size_t i = 0; i < len; i++) {
        put(e, data[i]);
    }
}

void cbor_text(cbor_enc_t *e, const char *s)
{
    size_t len = strlen(s);
    put_type_val(e, 3, len);
    for (size_t i = 0; i < len; i++) {
        put(e, (uint8_t)s[i]);
    }
}

void cbor_bool(cbor_enc_t *e, bool b)
{
    put(e, b ? 0xF5 : 0xF4);
}

/* ---------------- decoder ---------------- */

void cbor_dec_init(cbor_dec_t *d, const uint8_t *buf, size_t len)
{
    d->buf = buf;
    d->len = len;
    d->off = 0;
    d->ok = true;
}

static bool dec_byte(cbor_dec_t *d, uint8_t *b)
{
    if (!d->ok || d->off >= d->len) {
        d->ok = false;
        return false;
    }
    *b = d->buf[d->off++];
    return true;
}

static bool dec_bytes(cbor_dec_t *d, const uint8_t **p, size_t n)
{
    if (!d->ok || n > d->len - d->off) {
        d->ok = false;
        return false;
    }
    *p = d->buf + d->off;
    d->off += n;
    return true;
}

uint8_t cbor_peek(const cbor_dec_t *d)
{
    if (!d->ok || d->off >= d->len) return 0xFF;
    return d->buf[d->off] >> 5;
}

uint8_t cbor_read_head(cbor_dec_t *d, uint64_t *val)
{
    uint8_t b;
    if (!dec_byte(d, &b)) return 0xFF;

    uint8_t major = b >> 5;
    uint8_t ai = b & 0x1F;
    uint64_t v = 0;

    if (ai < 24) {
        v = ai;
    } else if (ai == 24) {
        uint8_t x;
        if (!dec_byte(d, &x)) return 0xFF;
        v = x;
    } else if (ai == 25) {
        const uint8_t *p;
        if (!dec_bytes(d, &p, 2)) return 0xFF;
        v = ((uint64_t)p[0] << 8) | p[1];
    } else if (ai == 26) {
        const uint8_t *p;
        if (!dec_bytes(d, &p, 4)) return 0xFF;
        v = ((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16) | ((uint64_t)p[2] << 8) | p[3];
    } else if (ai == 27) {
        const uint8_t *p;
        if (!dec_bytes(d, &p, 8)) return 0xFF;
        v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    } else {
        d->ok = false; /* indefinite lengths are not valid in CTAP2 */
        return 0xFF;
    }

    *val = v;
    return major;
}

bool cbor_read_uint(cbor_dec_t *d, uint64_t *out)
{
    uint64_t v;
    if (cbor_read_head(d, &v) != 0) return false;
    *out = v;
    return true;
}

bool cbor_read_int(cbor_dec_t *d, int64_t *out)
{
    uint64_t v;
    uint8_t major = cbor_read_head(d, &v);
    if (major == 0) {
        *out = (int64_t)v;
        return true;
    }
    if (major == 1) {
        *out = -1 - (int64_t)v;
        return true;
    }
    return false;
}

bool cbor_read_bool(cbor_dec_t *d, bool *out)
{
    uint64_t v;
    if (cbor_read_head(d, &v) != 7) return false;
    if (v == 20) { *out = false; return true; }
    if (v == 21) { *out = true; return true; }
    return false;
}

bool cbor_read_bytes(cbor_dec_t *d, const uint8_t **out, size_t *out_len)
{
    uint64_t n;
    if (cbor_read_head(d, &n) != 2) return false;
    return dec_bytes(d, out, (size_t)n) && (*out_len = (size_t)n, true);
}

bool cbor_read_text(cbor_dec_t *d, char *out, size_t cap)
{
    uint64_t n;
    const uint8_t *p;

    if (cbor_read_head(d, &n) != 3) return false;
    if (!dec_bytes(d, &p, (size_t)n)) return false;

    size_t copy = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
    memcpy(out, p, copy);
    out[copy] = '\0';
    return true;
}

bool cbor_read_map(cbor_dec_t *d, size_t *n)
{
    uint64_t v;
    if (cbor_read_head(d, &v) != 5) return false;
    *n = (size_t)v;
    return true;
}

bool cbor_read_array(cbor_dec_t *d, size_t *n)
{
    uint64_t v;
    if (cbor_read_head(d, &v) != 4) return false;
    *n = (size_t)v;
    return true;
}

bool cbor_skip(cbor_dec_t *d)
{
    uint64_t v;
    uint8_t major = cbor_read_head(d, &v);
    if (major == 0xFF) return false;

    switch (major) {
    case 0:
    case 1:
    case 7:
        return true;
    case 2:
    case 3: {
        const uint8_t *p;
        return dec_bytes(d, &p, (size_t)v);
    }
    case 4:
        for (uint64_t i = 0; i < v; i++) {
            if (!cbor_skip(d)) return false;
        }
        return true;
    case 5:
        for (uint64_t i = 0; i < 2 * v; i++) {
            if (!cbor_skip(d)) return false;
        }
        return true;
    case 6:
        return cbor_skip(d); /* tagged item */
    default:
        d->ok = false;
        return false;
    }
}
