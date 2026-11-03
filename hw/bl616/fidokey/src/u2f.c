/*
 * U2F (CTAP1) over CTAPHID_MSG: Register / Authenticate / Version
 *
 * Credentials live in the normal store (flag STORE_FLAG_U2F, key handle = 16-byte credential id), P-256 keys are
 * generated and used inside the FPGA. Registration is attested with the provisioned batch identity or the
 * shared default identity (see ctap2_attestation()).
 */
#include <string.h>
#include "ctap2.h"
#include "ctap2_int.h"
#include "fpga.h"
#include "store.h"
#include "u2f_att_default.h"

#define SW_OK 0x9000
#define SW_CONDITIONS_NOT_SATISFIED 0x6985
#define SW_WRONG_DATA 0x6A80
#define SW_WRONG_LENGTH 0x6700
#define SW_INS_NOT_SUPPORTED 0x6D00
#define SW_CLA_NOT_SUPPORTED 0x6E00
#define SW_UNKNOWN 0x6F00

static uint8_t u2f_out[2 + 1 + 65 + 1 + 16 + STORE_ATT_CERT_MAX + 80];


static void sw(uint32_t channel, uint16_t code)
{
    uint8_t r[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    ctaphid_send(channel, CTAPHID_MSG, r, 2);
}

/* ---------------- attestation identity ----------------
   Provisioned batch key + certificate when present, otherwise the shared default identity of u2f_att_default.h
   (identical on every unit, so attestation cannot be used to tell devices apart). */
static uint8_t att_cert_buf[STORE_ATT_CERT_MAX];

bool ctap2_attestation(uint8_t blob[64], const uint8_t **cert, size_t *certLen, bool *provisioned)
{
    static bool defaultReady;
    static uint8_t defaultBlob[64];
    uint8_t pub[64];
    size_t n = 0;

    if (store_att_load(blob, pub, att_cert_buf, &n)) {
        *cert = att_cert_buf;
        *certLen = n;
        if (provisioned) *provisioned = true;
        return true;
    }
    if (!defaultReady) {
        if (!fpga_p256_import(u2f_att_default_key, defaultBlob)) return false;
        defaultReady = true;
    }
    memcpy(blob, defaultBlob, 64);
    *cert = u2f_att_default_cert;
    *certLen = sizeof(u2f_att_default_cert);
    if (provisioned) *provisioned = false;
    return true;
}

/* PROVISION_ATTESTATION: private key (raw 32 bytes) + DER certificate that contains the matching public key */
bool ctap2_att_provision(const uint8_t priv[32], const uint8_t *cert, size_t certLen, const char **err)
{
    uint8_t blob[64], pub[64];
    if (certLen == 0 || certLen > STORE_ATT_CERT_MAX) { *err = "BAD_CERT_LENGTH"; return false; }
    if (!fpga_p256_import(priv, blob) || !fpga_p256_public(blob, pub)) { *err = "KEY_IMPORT_FAILED"; return false; }
    bool found = false;
    for (size_t i = 0; i + 64 <= certLen; i++) {
        if (memcmp(cert + i, pub, 64) == 0) { found = true; break; }
    }
    if (!found) { *err = "KEY_CERT_MISMATCH"; return false; }
    if (!store_att_save(blob, pub, cert, certLen)) { *err = "SAVE_FAILED"; return false; }
    return true;
}

/* ---------------- commands ---------------- */
static void u2f_register(uint32_t channel, const uint8_t *payload, size_t lc)
{
    if (lc != 64) { sw(channel, SW_WRONG_LENGTH); return; }
    const uint8_t *challenge = payload, *app = payload + 32;

    if (!ctap2_poll_up(true)) { sw(channel, SW_CONDITIONS_NOT_SATISFIED); return; } /* the client retries until a touch */

    uint8_t attBlob[64];
    const uint8_t *cert;
    size_t certLen = 0;
    fpga_set_keepalive(channel);
    if (!ctap2_attestation(attBlob, &cert, &certLen, NULL)) { fpga_set_keepalive(0); sw(channel, SW_UNKNOWN); return; }

    cred_record_t rec;
    memset(&rec, 0, sizeof(rec));
    uint8_t pub[64];
    if (!fpga_p256_genkey(rec.blob, pub)) { fpga_set_keepalive(0); sw(channel, SW_UNKNOWN); return; }
    rec.blobLen = 64;
    ctap2_random(rec.credId, STORE_CRED_ID_LEN);
    ctap2_random(rec.userId, 8); /* unique "user" so registrations never replace each other */
    rec.userIdLen = 8;
    memcpy(rec.rpIdHash, app, 32);
    rec.alg = -7;
    rec.flags = STORE_FLAG_U2F;
    rec.credProtect = 1;
    strcpy(rec.rpId, "u2f");
    if (!store_add(&rec)) { fpga_set_keepalive(0); sw(channel, SW_UNKNOWN); return; }

    uint8_t pub65[65];
    pub65[0] = 0x04;
    memcpy(pub65 + 1, pub, 64);

    uint8_t sd[1 + 32 + 32 + 16 + 65];
    sd[0] = 0x00;
    memcpy(sd + 1, app, 32);
    memcpy(sd + 33, challenge, 32);
    memcpy(sd + 65, rec.credId, 16);
    memcpy(sd + 81, pub65, 65);
    uint8_t digest[32], rs[64], sig[80];
    ctap2_sha256(sd, sizeof(sd), digest);
    bool ok = fpga_p256_sign(attBlob, digest, rs);
    fpga_set_keepalive(0);
    size_t sl = ok ? ctap2_der_ecdsa(rs, rs + 32, sig, sizeof(sig)) : 0;
    if (!sl) { sw(channel, SW_UNKNOWN); return; }

    size_t o = 0;
    u2f_out[o++] = 0x05;
    memcpy(u2f_out + o, pub65, 65); o += 65;
    u2f_out[o++] = 16;
    memcpy(u2f_out + o, rec.credId, 16); o += 16;
    memcpy(u2f_out + o, cert, certLen); o += certLen;
    memcpy(u2f_out + o, sig, sl); o += sl;
    u2f_out[o++] = 0x90;
    u2f_out[o++] = 0x00;
    ctaphid_send(channel, CTAPHID_MSG, u2f_out, (uint16_t)o);
}

static void u2f_authenticate(uint32_t channel, uint8_t p1, const uint8_t *payload, size_t lc)
{
    if (lc < 65) { sw(channel, SW_WRONG_LENGTH); return; }
    const uint8_t *challenge = payload, *app = payload + 32;
    size_t khLen = payload[64];
    if (lc != 65 + khLen) { sw(channel, SW_WRONG_LENGTH); return; }

    cred_record_t rec;
    if (khLen != STORE_CRED_ID_LEN || !store_find_by_credid(payload + 65, &rec) ||
        memcmp(rec.rpIdHash, app, 32) != 0 || rec.alg != -7) {
        sw(channel, SW_WRONG_DATA);
        return;
    }
    if (p1 == 0x07) { /* check-only: the key handle is ours */
        sw(channel, SW_CONDITIONS_NOT_SATISFIED);
        return;
    }
    if (p1 != 0x03 && p1 != 0x08) { sw(channel, SW_WRONG_DATA); return; }
    bool present = ctap2_poll_up(p1 == 0x03);       /* 0x08: sign without a touch, reporting whether there was one */
    if (p1 == 0x03 && !present) { sw(channel, SW_CONDITIONS_NOT_SATISFIED); return; }

    uint32_t counter = rec.signCount + 1;
    store_update_sign_count(rec.credId, counter);

    uint8_t sd[32 + 1 + 4 + 32];
    memcpy(sd, app, 32);
    sd[32] = present ? 0x01 : 0x00; /* user presence */
    sd[33] = (uint8_t)(counter >> 24);
    sd[34] = (uint8_t)(counter >> 16);
    sd[35] = (uint8_t)(counter >> 8);
    sd[36] = (uint8_t)counter;
    memcpy(sd + 37, challenge, 32);

    uint8_t digest[32], rs[64], sig[80];
    ctap2_sha256(sd, sizeof(sd), digest);
    fpga_set_keepalive(channel);
    bool ok = fpga_p256_sign(rec.blob, digest, rs);
    fpga_set_keepalive(0);
    size_t sl = ok ? ctap2_der_ecdsa(rs, rs + 32, sig, sizeof(sig)) : 0;
    if (!sl) { sw(channel, SW_UNKNOWN); return; }

    size_t o = 0;
    u2f_out[o++] = present ? 0x01 : 0x00;
    memcpy(u2f_out + o, sd + 33, 4); o += 4;
    memcpy(u2f_out + o, sig, sl); o += sl;
    u2f_out[o++] = 0x90;
    u2f_out[o++] = 0x00;
    ctaphid_send(channel, CTAPHID_MSG, u2f_out, (uint16_t)o);
}

void ctap2_u2f_handle(uint32_t channel, const uint8_t *data, size_t len)
{
    if (len < 4) { sw(channel, SW_WRONG_LENGTH); return; }
    uint8_t cla = data[0], ins = data[1], p1 = data[2];
    if (cla != 0x00) { sw(channel, SW_CLA_NOT_SUPPORTED); return; }

    /* APDU body: none, short (Lc byte) or extended (00 Lc Lc) */
    const uint8_t *payload = data + 4;
    size_t lc = 0;
    if (len >= 7 && data[4] == 0x00) {
        lc = ((size_t)data[5] << 8) | data[6];
        payload = data + 7;
    } else if (len >= 5) {
        lc = data[4];
        payload = data + 5;
    }
    if (lc > len || (size_t)(payload - data) + lc > len) { sw(channel, SW_WRONG_LENGTH); return; }

    switch (ins) {
    case 0x03: { /* U2F_VERSION */
        static const uint8_t version[] = { 'U', '2', 'F', '_', 'V', '2', 0x90, 0x00 };
        ctaphid_send(channel, CTAPHID_MSG, version, sizeof(version));
        return;
    }
    case 0x01:
        u2f_register(channel, payload, lc);
        return;
    case 0x02:
        u2f_authenticate(channel, p1, payload, lc);
        return;
    default:
        sw(channel, SW_INS_NOT_SUPPORTED);
        return;
    }
}
