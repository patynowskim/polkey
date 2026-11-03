/*
 * Minimal CBOR encoder for CTAP2 responses (RFC 8949, canonical form)
 */
#ifndef CBOR_H
#define CBOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t off;
    bool ok;
} cbor_enc_t;

void cbor_init(cbor_enc_t *e, uint8_t *buf, size_t cap);

void cbor_map(cbor_enc_t *e, size_t n);
void cbor_array(cbor_enc_t *e, size_t n);
void cbor_uint(cbor_enc_t *e, uint64_t v);
void cbor_negint(cbor_enc_t *e, int64_t v);
void cbor_bytes(cbor_enc_t *e, const uint8_t *data, size_t len);
void cbor_text(cbor_enc_t *e, const char *s);
void cbor_bool(cbor_enc_t *e, bool b);

static inline size_t cbor_len(const cbor_enc_t *e)
{
    return e->off;
}

/* ---------------- decoder ---------------- */

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t off;
    bool ok;
} cbor_dec_t;

void cbor_dec_init(cbor_dec_t *d, const uint8_t *buf, size_t len);

/* Major type of the next item without consuming it; 0xFF on error */
uint8_t cbor_peek(const cbor_dec_t *d);

/* Consume one item header; returns the major type (0..7) or 0xFF on error,
   with the header's value (count/length) in *val. */
uint8_t cbor_read_head(cbor_dec_t *d, uint64_t *val);

bool cbor_read_uint(cbor_dec_t *d, uint64_t *out);
bool cbor_read_int(cbor_dec_t *d, int64_t *out);
bool cbor_read_bool(cbor_dec_t *d, bool *out);
bool cbor_read_bytes(cbor_dec_t *d, const uint8_t **out, size_t *out_len);
bool cbor_read_text(cbor_dec_t *d, char *out, size_t cap); /* NUL-terminates */
bool cbor_read_map(cbor_dec_t *d, size_t *n);
bool cbor_read_array(cbor_dec_t *d, size_t *n);
bool cbor_skip(cbor_dec_t *d);

#endif
