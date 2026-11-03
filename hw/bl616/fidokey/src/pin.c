#include <string.h>
#include "pin.h"
#include "cbor.h"
#include "aes.h"
#include "sha256.h"
#include "fpga.h"
#include "store.h"
#include "ctap2.h"

#define PIN_MAX_RETRIES 8
#define PIN_MAX_CONSECUTIVE 3

static store_meta_t meta;

/* authenticator key agreement key (FPGA-wrapped) */
static uint8_t ka_blob[FPGA_BLOB_LEN];
static uint8_t ka_pub[64];
static bool ka_valid;

/* pinUvAuthToken */
static struct {
    bool valid;
    uint8_t token[32];
    uint8_t perms;
    bool rpBound;
    uint8_t rpIdHash[32];
} tok;

static uint8_t consecutive_failures;

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void pin_init(void)
{
    store_meta_load(&meta);
    ka_valid = false;
    tok.valid = false;
    consecutive_failures = 0;
}

void pin_factory_reset(void)
{
    memset(&meta, 0, sizeof(meta));
    meta.retries = PIN_MAX_RETRIES;
    ka_valid = false;
    tok.valid = false;
    consecutive_failures = 0;
}

bool pin_is_set(void)
{
    return meta.pinSet == 1;
}

static bool regenerate_key_agreement(void)
{
    ka_valid = fpga_p256_genkey(ka_blob, ka_pub);
    return ka_valid;
}

static void new_token(void)
{
    ctap2_random(tok.token, sizeof(tok.token));
    tok.valid = true;
}

/* ---------------- shared secret ---------------- */
bool pin_shared_secret(uint8_t protocol, const uint8_t peerXY[64], pin_secret_t *out)
{
    if ((protocol != 1 && protocol != 2) || !ka_valid) return false;
    uint8_t z[32];
    if (!fpga_p256_ecdh(ka_blob, peerXY, z)) return false;
    out->protocol = protocol;
    if (protocol == 1) {
        sha256_ctx_t c;
        uint8_t h[32];
        sha256_begin(&c);
        sha256_feed(&c, z, 32);
        sha256_end(&c, h);
        memcpy(out->hmacKey, h, 32);
        memcpy(out->aesKey, h, 32);
        memset(h, 0, sizeof(h));
    } else {
        hkdf_sha256_32(NULL, 0, z, 32, "CTAP2 HMAC key", out->hmacKey);
        hkdf_sha256_32(NULL, 0, z, 32, "CTAP2 AES key", out->aesKey);
    }
    memset(z, 0, sizeof(z));
    return true;
}

bool pin_secret_auth_ok(const pin_secret_t *s, const uint8_t *msg, size_t msgLen, const uint8_t *auth, size_t authLen)
{
    size_t want = (s->protocol == 1) ? 16 : 32;
    if (authLen != want) return false;
    uint8_t mac[32];
    hmac_sha256(s->hmacKey, 32, msg, msgLen, NULL, 0, mac);
    return ct_equal(mac, auth, want);
}

size_t pin_encrypt(const pin_secret_t *s, const uint8_t *in, size_t len, uint8_t *out)
{
    if (len % 16) return 0;
    if (s->protocol == 1) {
        uint8_t iv[16] = { 0 };
        aes256_cbc_encrypt(s->aesKey, iv, in, len, out);
        return len;
    }
    uint8_t iv[16];
    ctap2_random(iv, 16);
    memcpy(out, iv, 16);
    aes256_cbc_encrypt(s->aesKey, iv, in, len, out + 16);
    return len + 16;
}

size_t pin_decrypt(const pin_secret_t *s, const uint8_t *in, size_t len, uint8_t *out)
{
    if (s->protocol == 1) {
        if (len == 0 || len % 16) return 0;
        uint8_t iv[16] = { 0 };
        aes256_cbc_decrypt(s->aesKey, iv, in, len, out);
        return len;
    }
    if (len < 32 || len % 16) return 0;
    aes256_cbc_decrypt(s->aesKey, in, in + 16, len - 16, out);
    return len - 16;
}

/* ---------------- PIN verification ---------------- */
static void make_verifier(const uint8_t salt[16], const uint8_t pinHash16[16], uint8_t out[32])
{
    sha256_ctx_t c;
    sha256_begin(&c);
    sha256_feed(&c, salt, 16);
    sha256_feed(&c, pinHash16, 16);
    sha256_end(&c, out);
}

/* Decrement the counter first (persisted), then compare; restore on success. */
static uint8_t check_pin_hash(const pin_secret_t *s, const uint8_t *pinHashEnc, size_t encLen)
{
    if (meta.retries == 0) return CTAP2_ERR_PIN_BLOCKED;
    if (consecutive_failures >= PIN_MAX_CONSECUTIVE) return CTAP2_ERR_PIN_AUTH_BLOCKED;

    uint8_t plain[32];
    size_t n = pin_decrypt(s, pinHashEnc, encLen, plain);
    if (n != 16) return CTAP2_ERR_PIN_INVALID;

    meta.retries--;
    store_meta_save(&meta);

    uint8_t v[32];
    make_verifier(meta.pinSalt, plain, v);
    memset(plain, 0, sizeof(plain));

    if (ct_equal(v, meta.pinVerifier, 32)) {
        meta.retries = PIN_MAX_RETRIES;
        store_meta_save(&meta);
        consecutive_failures = 0;
        return CTAP2_OK;
    }

    regenerate_key_agreement(); /* the platform must fetch a fresh key before the next attempt */
    consecutive_failures++;
    if (meta.retries == 0) return CTAP2_ERR_PIN_BLOCKED;
    if (consecutive_failures >= PIN_MAX_CONSECUTIVE) return CTAP2_ERR_PIN_AUTH_BLOCKED;
    return CTAP2_ERR_PIN_INVALID;
}

/* newPinEnc -> verifier. Returns status. */
static uint8_t install_new_pin(const pin_secret_t *s, const uint8_t *newPinEnc, size_t encLen)
{
    uint8_t plain[80];
    size_t n = pin_decrypt(s, newPinEnc, encLen, plain);
    if (n != 64) return CTAP2_ERR_INVALID_PARAMETER;

    size_t pinLen = 0;
    while (pinLen < 64 && plain[pinLen] != 0) pinLen++;
    if (pinLen == 64 || pinLen < 4) {
        memset(plain, 0, sizeof(plain));
        return CTAP2_ERR_PIN_POLICY_VIOLATION;
    }

    uint8_t h[32];
    sha256_ctx_t c;
    sha256_begin(&c);
    sha256_feed(&c, plain, pinLen);
    sha256_end(&c, h);
    memset(plain, 0, sizeof(plain));

    ctap2_random(meta.pinSalt, sizeof(meta.pinSalt));
    make_verifier(meta.pinSalt, h, meta.pinVerifier);
    memset(h, 0, sizeof(h));
    meta.pinSet = 1;
    meta.retries = PIN_MAX_RETRIES;
    consecutive_failures = 0;
    return store_meta_save(&meta) ? CTAP2_OK : CTAP2_ERR_PROCESSING;
}

/* ---------------- token ---------------- */
uint8_t pin_verify_token_auth(uint8_t protocol, const uint8_t *msg, size_t msgLen,
                              const uint8_t *auth, size_t authLen, uint8_t perm,
                              const uint8_t rpIdHash[32])
{
    if (protocol != 1 && protocol != 2) return CTAP2_ERR_INVALID_PARAMETER;
    if (!tok.valid) return CTAP2_ERR_PIN_AUTH_INVALID;
    size_t want = (protocol == 1) ? 16 : 32;
    if (authLen != want) return CTAP2_ERR_PIN_AUTH_INVALID;
    uint8_t mac[32];
    hmac_sha256(tok.token, 32, msg, msgLen, NULL, 0, mac);
    if (!ct_equal(mac, auth, want)) return CTAP2_ERR_PIN_AUTH_INVALID;
    if (!(tok.perms & perm)) return CTAP2_ERR_PIN_AUTH_INVALID;
    if (tok.rpBound && !ct_equal(tok.rpIdHash, rpIdHash, 32)) return CTAP2_ERR_PIN_AUTH_INVALID;
    return CTAP2_OK;
}

void pin_token_consume(void)
{
    tok.valid = false;
    memset(tok.token, 0, sizeof(tok.token));
}

/* ---------------- authenticatorClientPIN ---------------- */
static bool read_cose_key(cbor_dec_t *d, uint8_t xy[64])
{
    size_t m;
    if (!cbor_read_map(d, &m)) return false;
    bool gotX = false, gotY = false;
    for (size_t i = 0; i < m && d->ok; i++) {
        int64_t k;
        if (!cbor_read_int(d, &k)) return false;
        if (k == -2 || k == -3) {
            const uint8_t *p;
            size_t n;
            if (!cbor_read_bytes(d, &p, &n) || n != 32) return false;
            memcpy(xy + (k == -2 ? 0 : 32), p, 32);
            if (k == -2) gotX = true; else gotY = true;
        } else if (!cbor_skip(d)) {
            return false;
        }
    }
    return d->ok && gotX && gotY;
}

uint8_t pin_client_pin(const uint8_t *data, size_t len, uint8_t *out, size_t outCap, size_t *outLen)
{
    uint64_t protocol = 0, sub = 0, perms = 0;
    uint8_t peer[64];
    bool havePeer = false;
    const uint8_t *auth = NULL, *newPinEnc = NULL, *pinHashEnc = NULL;
    size_t authLen = 0, newPinEncLen = 0, pinHashEncLen = 0;
    char rpId[128] = { 0 };
    bool haveRp = false;

    cbor_dec_t d;
    cbor_dec_init(&d, data, len);
    size_t m;
    if (!cbor_read_map(&d, &m)) return CTAP2_ERR_INVALID_CBOR;

    for (size_t i = 0; i < m && d.ok; i++) {
        uint64_t k;
        if (!cbor_read_uint(&d, &k)) return CTAP2_ERR_INVALID_CBOR;
        switch (k) {
        case 1: if (!cbor_read_uint(&d, &protocol)) d.ok = false; break;
        case 2: if (!cbor_read_uint(&d, &sub)) d.ok = false; break;
        case 3: havePeer = read_cose_key(&d, peer); if (!havePeer) d.ok = false; break;
        case 4: if (!cbor_read_bytes(&d, &auth, &authLen)) d.ok = false; break;
        case 5: if (!cbor_read_bytes(&d, &newPinEnc, &newPinEncLen)) d.ok = false; break;
        case 6: if (!cbor_read_bytes(&d, &pinHashEnc, &pinHashEncLen)) d.ok = false; break;
        case 9: if (!cbor_read_uint(&d, &perms)) d.ok = false; break;
        case 10: haveRp = cbor_read_text(&d, rpId, sizeof(rpId)); if (!haveRp) d.ok = false; break;
        default: if (!cbor_skip(&d)) d.ok = false; break;
        }
    }
    if (!d.ok) return CTAP2_ERR_INVALID_CBOR;
    if (sub == 0) return CTAP2_ERR_MISSING_PARAMETER;

    cbor_enc_t e;
    cbor_init(&e, out, outCap);

    if (sub == 0x01) { /* getPinRetries */
        bool blocked = consecutive_failures >= PIN_MAX_CONSECUTIVE;
        cbor_map(&e, blocked ? 2 : 1);
        cbor_uint(&e, 3);
        cbor_uint(&e, meta.retries);
        if (blocked) {
            cbor_uint(&e, 4);
            cbor_bool(&e, true);
        }
        *outLen = cbor_len(&e);
        return e.ok ? CTAP2_OK : CTAP2_ERR_PROCESSING;
    }

    if (protocol != 1 && protocol != 2) {
        return protocol == 0 ? CTAP2_ERR_MISSING_PARAMETER : CTAP2_ERR_INVALID_PARAMETER;
    }

    if (sub == 0x02) { /* getKeyAgreement */
        if (!ka_valid && !regenerate_key_agreement()) return CTAP2_ERR_PROCESSING;
        cbor_map(&e, 1);
        cbor_uint(&e, 1);
        cbor_map(&e, 5);
        cbor_uint(&e, 1);
        cbor_uint(&e, 2);          /* kty: EC2 */
        cbor_uint(&e, 3);
        cbor_negint(&e, -25);      /* alg: ECDH-ES+HKDF-256 */
        cbor_negint(&e, -1);
        cbor_uint(&e, 1);          /* crv: P-256 */
        cbor_negint(&e, -2);
        cbor_bytes(&e, ka_pub, 32);
        cbor_negint(&e, -3);
        cbor_bytes(&e, ka_pub + 32, 32);
        *outLen = cbor_len(&e);
        return e.ok ? CTAP2_OK : CTAP2_ERR_PROCESSING;
    }

    if (sub != 0x03 && sub != 0x04 && sub != 0x05 && sub != 0x09) {
        return CTAP2_ERR_UNSUPPORTED_OPTION;
    }
    if (!havePeer) return CTAP2_ERR_MISSING_PARAMETER;

    pin_secret_t secret;
    if (!pin_shared_secret((uint8_t)protocol, peer, &secret)) return CTAP2_ERR_PROCESSING;
    uint8_t st = CTAP2_OK;

    if (sub == 0x03) { /* setPIN */
        if (!newPinEnc || !auth) return CTAP2_ERR_MISSING_PARAMETER;
        if (pin_is_set()) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_NOT_ALLOWED; }
        if (!pin_secret_auth_ok(&secret, newPinEnc, newPinEncLen, auth, authLen)) st = CTAP2_ERR_PIN_AUTH_INVALID;
        else st = install_new_pin(&secret, newPinEnc, newPinEncLen);
        memset(&secret, 0, sizeof(secret));
        *outLen = 0;
        return st;
    }

    if (sub == 0x04) { /* changePIN */
        if (!newPinEnc || !pinHashEnc || !auth) return CTAP2_ERR_MISSING_PARAMETER;
        if (!pin_is_set()) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_PIN_NOT_SET; }
        if (meta.retries == 0) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_PIN_BLOCKED; }
        if (consecutive_failures >= PIN_MAX_CONSECUTIVE) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_PIN_AUTH_BLOCKED; }
        uint8_t buf[160];
        if (newPinEncLen + pinHashEncLen > sizeof(buf)) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_INVALID_LENGTH; }
        memcpy(buf, newPinEnc, newPinEncLen);
        memcpy(buf + newPinEncLen, pinHashEnc, pinHashEncLen);
        if (!pin_secret_auth_ok(&secret, buf, newPinEncLen + pinHashEncLen, auth, authLen)) {
            st = CTAP2_ERR_PIN_AUTH_INVALID;
        } else {
            st = check_pin_hash(&secret, pinHashEnc, pinHashEncLen);
            if (st == CTAP2_OK) st = install_new_pin(&secret, newPinEnc, newPinEncLen);
        }
        memset(&secret, 0, sizeof(secret));
        memset(buf, 0, sizeof(buf));
        *outLen = 0;
        return st;
    }

    /* getPinToken (0x05) / getPinUvAuthTokenUsingPinWithPermissions (0x09) */
    if (!pinHashEnc) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_MISSING_PARAMETER; }
    if (!pin_is_set()) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_PIN_NOT_SET; }
    if (sub == 0x09) {
        if (perms == 0) { memset(&secret, 0, sizeof(secret)); return CTAP2_ERR_MISSING_PARAMETER; }
        if (perms & ~(uint64_t)(PIN_PERM_MC | PIN_PERM_GA | PIN_PERM_CM | PIN_PERM_LBW | PIN_PERM_ACFG)) {
            memset(&secret, 0, sizeof(secret));
            return CTAP2_ERR_UNAUTHORIZED_PERMISSION;
        }
        if ((perms & (PIN_PERM_MC | PIN_PERM_GA)) && !haveRp) {
            memset(&secret, 0, sizeof(secret));
            return CTAP2_ERR_MISSING_PARAMETER;
        }
    } else {
        perms = PIN_PERM_MC | PIN_PERM_GA;
        haveRp = false;
    }

    st = check_pin_hash(&secret, pinHashEnc, pinHashEncLen);
    if (st != CTAP2_OK) { memset(&secret, 0, sizeof(secret)); return st; }

    new_token();
    tok.perms = (uint8_t)perms;
    tok.rpBound = haveRp;
    if (haveRp) {
        sha256_ctx_t c;
        sha256_begin(&c);
        sha256_feed(&c, (const uint8_t *)rpId, strlen(rpId));
        sha256_end(&c, tok.rpIdHash);
    }

    uint8_t enc[48];
    size_t encLen = pin_encrypt(&secret, tok.token, 32, enc);
    memset(&secret, 0, sizeof(secret));
    cbor_map(&e, 1);
    cbor_uint(&e, 2);
    cbor_bytes(&e, enc, encLen);
    *outLen = cbor_len(&e);
    return e.ok ? CTAP2_OK : CTAP2_ERR_PROCESSING;
}
