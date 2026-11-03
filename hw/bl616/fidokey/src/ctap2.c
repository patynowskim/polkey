/*
 * CTAP2 command handling for the PolKey FIDO2 authenticator (BL616)
 *
 * GetInfo, MakeCredential and GetAssertion. Private keys are generated and
 * used inside the FPGA accelerator; only wrapped blobs are stored on the BL616.
 * Attestation is self-attestation (packed) for now.
 */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_core.h"
#include "sha256.h"
#include "pin.h"
#include "vault.h"
#include "dirtyjtag.h"
#include "ctap2_int.h"
#include "bflb_efuse.h"
#include "bflb_sec_trng.h"
#include "ctaphid.h"
#include "ctap2.h"
#include "cbor.h"
#include "fpga.h"
#include "fp.h"
#include "oled.h"
#include "store.h"

#define CTAP2_MAX_RESP CTAPHID_MAX_MSG

uint8_t ctap2_resp[CTAP2_MAX_RESP];
#define AUTHDATA_MAX 3072 /* ML-DSA-87: 2592-byte public key */
static uint8_t authData[AUTHDATA_MAX];
static uint8_t sign_msg[AUTHDATA_MAX + 32];
static uint8_t sig_buf[4700];                /* ML-DSA-87 signature: 4627 bytes */
static uint8_t big_blob[STORE_BIG_MAX];      /* wrapped RSA / ML-DSA key loaded for signing */

static TickType_t boot_tick;

/* ---------------- user presence (button S2 on the FPGA board) ---------------- */
static bool up_prompt_on;
static TickType_t up_prompt_until; /* 0 = until switched off explicitly */

static void up_led(bool on)
{
    if (up_prompt_on == on) return;
    if (fpga_ux_led(on ? 0x87 : 0x00)) up_prompt_on = on; /* all LEDs blink while a touch is wanted */
}

/* an enrolled fingerprint also proves presence (the sensor needs a live finger and a template match) */
static bool fp_ready(void)
{
    return fpga_pf_supported() && fp_present() && fp_enrolled() > 0;
}

static bool fp_touch(void)
{
    uint16_t sc;
    return fp_poll(&sc) == FP_MATCH;
}

bool ctap2_up_required(void)
{
    return vault_up_required() && (fpga_ux_supported() || fp_ready());
}

int ctap2_wait_up(uint32_t channel, uint32_t timeout_ms)
{
    if (!ctap2_up_required()) return UP_OK;
    uint8_t st = 0;
    fpga_ux_buttons(true, &st); /* a press from before this request does not count */
    up_prompt_until = 0;
    up_led(true);
    bool useFp = fp_ready();
    oled_show(useFp ? "Touch sensor or" : "Press button", useFp ? "press button" : "to confirm");

    TickType_t start = xTaskGetTickCount(), lastKa = start - pdMS_TO_TICKS(100);
    int res = UP_TIMEOUT;
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (channel && ctaphid_cancel_requested()) { res = UP_CANCELLED; break; }
        if (fpga_ux_buttons(true, &st) && (st & 2)) { res = UP_OK; break; } /* the latch is set by a new press only */
        if (useFp && fp_touch()) { res = UP_OK; break; }
        if (channel && (xTaskGetTickCount() - lastKa) >= pdMS_TO_TICKS(100)) {
            uint8_t ka = CTAPHID_STATUS_UPNEEDED;
            ctaphid_send(channel, CTAPHID_KEEPALIVE, &ka, 1);
            lastKa = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    up_led(false);
    oled_show(res == UP_OK ? "Confirmed" : "Timeout", "");
    return res;
}

bool ctap2_poll_up(bool prompt)
{
    if (!ctap2_up_required()) return true;
    uint8_t st = 0;
    if ((fpga_ux_buttons(true, &st) && (st & 2)) || (fp_ready() && fp_touch())) {
        up_led(false);
        return true;
    }
    if (prompt) {
        oled_show("Touch sensor or", "press button");
        up_prompt_until = xTaskGetTickCount() + pdMS_TO_TICKS(1000); /* the client polls again within that time */
        up_led(true);
    }
    return false;
}

void ctap2_ux_housekeeping(void)
{
    if (up_prompt_on && up_prompt_until && (int32_t)(xTaskGetTickCount() - up_prompt_until) > 0) up_led(false);
    oled_tick();
}

void ctap2_init(void)
{
    boot_tick = xTaskGetTickCount();
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void ctap2_sha256(const uint8_t *data, size_t len, uint8_t out[32]) { sha256(data, len, out); }

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_begin(&ctx);
    if (len) sha256_feed(&ctx, data, len);
    sha256_end(&ctx, out);
}

static void sha256_2(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_begin(&ctx);
    if (alen) sha256_feed(&ctx, a, alen);
    if (blen) sha256_feed(&ctx, b, blen);
    sha256_end(&ctx, out);
}

void ctap2_random(uint8_t *out, size_t len)
{
    static struct bflb_device_s *trng;
    static uint32_t fallback;
    uint8_t r[32];

    if (!trng) trng = bflb_device_get_by_name("sec_trng");

    while (len) {
        if (trng && bflb_trng_read(trng, r) == 0) {
            /* fresh entropy */
        } else {
            fallback++;
            uint8_t chipid[8] = { 0 };
            bflb_efuse_get_chipid(chipid);
            sha256_2(chipid, sizeof(chipid), (uint8_t *)&fallback, sizeof(fallback), r);
        }
        size_t n = len < 32 ? len : 32;
        if (fpga_available()) { /* mix in the FPGA TRNG: never weaker than the BL616 TRNG alone */
            uint8_t f[32];
            if (fpga_fill_random(f, n)) {
                for (size_t i = 0; i < n; i++) r[i] ^= f[i];
            }
        }
        memcpy(out, r, n);
        out += n;
        len -= n;
    }
}

/* Same AAGUID namespace as the ESP32 firmware, with the chip UID in the tail */
static void get_aaguid(uint8_t out[16])
{
    static const uint8_t prefix[10] = {
        0x4F, 0xA2, 0xB1, 0x3C, 0x7C, 0x89, 0x4E, 0x5B, 0xBC, 0x6D
    };
    uint8_t chipid[8] = { 0 };

    memcpy(out, prefix, sizeof(prefix));
    if (bflb_efuse_get_chipid(chipid) != 0) {
        memset(chipid, 0xAA, sizeof(chipid));
    }
    memcpy(out + sizeof(prefix), chipid + 2, 6);
}

/* ASN.1 DER SEQUENCE{INTEGER r, INTEGER s} from raw r||s */
size_t ctap2_der_ecdsa(const uint8_t r[32], const uint8_t s[32], uint8_t *out, size_t cap);
static size_t der_encode_ecdsa(const uint8_t r[32], const uint8_t s[32], uint8_t *out, size_t cap)
{
    size_t ri = 0, si = 0;
    while (ri < 31 && r[ri] == 0) ri++;
    while (si < 31 && s[si] == 0) si++;

    size_t rlen = 32 - ri, slen = 32 - si;
    size_t rpad = (r[ri] & 0x80) ? 1 : 0;
    size_t spad = (s[si] & 0x80) ? 1 : 0;
    size_t body = 2 + rpad + rlen + 2 + spad + slen;

    if (body + 2 > cap) return 0;

    size_t o = 0;
    out[o++] = 0x30;
    out[o++] = (uint8_t)body;
    out[o++] = 0x02;
    out[o++] = (uint8_t)(rpad + rlen);
    if (rpad) out[o++] = 0;
    memcpy(out + o, r + ri, rlen);
    o += rlen;
    out[o++] = 0x02;
    out[o++] = (uint8_t)(spad + slen);
    if (spad) out[o++] = 0;
    memcpy(out + o, s + si, slen);
    o += slen;
    return o;
}

size_t ctap2_der_ecdsa(const uint8_t r[32], const uint8_t s[32], uint8_t *out, size_t cap)
{
    return der_encode_ecdsa(r, s, out, cap);
}

static void put_alg(cbor_enc_t *e, int64_t alg)
{
    cbor_map(e, 2);
    cbor_text(e, "alg");
    cbor_negint(e, alg);
    cbor_text(e, "type");
    cbor_text(e, "public-key");
}

static void build_get_info(cbor_enc_t *e)
{
    cbor_map(e, 13);

    cbor_uint(e, 0x01); /* versions */
    cbor_array(e, 3);
    cbor_text(e, "FIDO_2_0");
    cbor_text(e, "FIDO_2_1_PRE");
    cbor_text(e, "FIDO_2_1");

    cbor_uint(e, 0x02); /* extensions */
    cbor_array(e, 4);
    cbor_text(e, "credBlob");
    cbor_text(e, "credProtect");
    cbor_text(e, "hmac-secret");
    cbor_text(e, "largeBlobKey");

    cbor_uint(e, 0x03); /* aaguid */
    uint8_t aaguid[16];
    get_aaguid(aaguid);
    cbor_bytes(e, aaguid, sizeof(aaguid));

    cbor_uint(e, 0x04); /* options (canonical order) */
    cbor_map(e, 7);
    cbor_text(e, "rk");
    cbor_bool(e, true);
    cbor_text(e, "up");
    cbor_bool(e, true);
    cbor_text(e, "uv");
    cbor_bool(e, false);
    cbor_text(e, "credMgmt");
    cbor_bool(e, true);
    cbor_text(e, "clientPin");
    cbor_bool(e, pin_is_set());
    cbor_text(e, "largeBlobs");
    cbor_bool(e, true);
    cbor_text(e, "pinUvAuthToken");
    cbor_bool(e, true);

    cbor_uint(e, 0x05); /* maxMsgSize */
    cbor_uint(e, CTAPHID_MAX_MSG);

    cbor_uint(e, 0x06); /* pinProtocols */
    cbor_array(e, 2);
    cbor_uint(e, 2);
    cbor_uint(e, 1);

    cbor_uint(e, 0x07); /* maxCredentialCountInList */
    cbor_uint(e, 8);

    cbor_uint(e, 0x08); /* maxCredentialIdLength */
    cbor_uint(e, 32);

    cbor_uint(e, 0x09); /* transports */
    cbor_array(e, 1);
    cbor_text(e, "usb");

    cbor_uint(e, 0x0A); /* algorithms */
    cbor_array(e, 6);
    put_alg(e, -7);   /* ES256 */
    put_alg(e, -8);   /* EdDSA */
    put_alg(e, -257); /* RS256 */
    put_alg(e, -48);  /* ML-DSA-44 */
    put_alg(e, -49);  /* ML-DSA-65 */
    put_alg(e, -50);  /* ML-DSA-87 */

    cbor_uint(e, 0x0B); /* maxSerializedLargeBlobArray */
    cbor_uint(e, STORE_LB_MAX);

    cbor_uint(e, 0x0D); /* minPINLength */
    cbor_uint(e, 4);

    cbor_uint(e, 0x0F); /* maxCredBlobLength */
    cbor_uint(e, STORE_CREDBLOB_MAX);
}

static void send_status(uint32_t channel, uint8_t status);
void ctap2_send_status(uint32_t channel, uint8_t status) { send_status(channel, status); }

static void send_status(uint32_t channel, uint8_t status)
{
    ctap2_resp[0] = status;
    ctaphid_send(channel, CTAPHID_CBOR, ctap2_resp, 1);
}

static int mldsa_set(int alg)
{
    return alg == -48 ? 44 : alg == -49 ? 65 : alg == -50 ? 87 : 0;
}

/* Sign msg with the credential key; returns the signature length (ES256: DER), 0 on failure */
static size_t credential_sign(const cred_record_t *rec, const uint8_t *msg, size_t msgLen, uint8_t *sigOut, size_t sigCap)
{
    if (rec->alg == -7) {
        uint8_t digest[32], rs[64];
        sha256(msg, msgLen, digest);
        if (!fpga_p256_sign(rec->blob, digest, rs)) return 0;
        return der_encode_ecdsa(rs, rs + 32, sigOut, sigCap);
    }
    if (rec->alg == -8) {
        if (sigCap < 64 || !fpga_ed25519_sign(rec->blob, msg, msgLen, sigOut)) return 0;
        return 64;
    }
    if (rec->alg == -257) {
        uint8_t digest[32];
        if (sigCap < 256 || rec->blobLen != FPGA_RSA_BLOB_LEN ||
            !store_big_read(rec, big_blob, sizeof(big_blob))) return 0;
        sha256(msg, msgLen, digest);
        bool ok = fpga_rsa_sign_sha256(big_blob, digest, sigOut);
        memset(big_blob, 0, FPGA_RSA_BLOB_LEN);
        return ok ? 256 : 0;
    }
    int ps = mldsa_set(rec->alg);
    if (ps) {
        size_t sl = 0;
        if (rec->blobLen != fpga_mldsa_blob_len(ps) || !store_big_read(rec, big_blob, sizeof(big_blob))) return 0;
        bool ok = fpga_mldsa_sign(ps, big_blob, msg, msgLen, sigOut, &sl);
        memset(big_blob, 0, rec->blobLen);
        return ok && sl <= sigCap ? sl : 0;
    }
    return 0;
}

static bool alg_supported(int64_t a)
{
    if (a == -7 || a == -8) return true;
    if (a == -257) return fpga_has_rsa();
    if (a == -48 || a == -49 || a == -50) return fpga_has_pq();
    return false;
}

/* COSE_Key for a credential public key: P-256 X||Y (64), Ed25519 (32), RSA n (256), ML-DSA pk */
void ctap2_put_cose_key(cbor_enc_t *e, int alg, const uint8_t *pub, size_t pubLen)
{
    if (alg == -7) {
        cbor_map(e, 5);
        cbor_uint(e, 1);
        cbor_uint(e, 2); /* kty: EC2 */
        cbor_uint(e, 3);
        cbor_negint(e, -7);
        cbor_negint(e, -1);
        cbor_uint(e, 1); /* crv: P-256 */
        cbor_negint(e, -2);
        cbor_bytes(e, pub, 32);
        cbor_negint(e, -3);
        cbor_bytes(e, pub + 32, 32);
    } else if (alg == -8) {
        cbor_map(e, 4);
        cbor_uint(e, 1);
        cbor_uint(e, 1); /* kty: OKP */
        cbor_uint(e, 3);
        cbor_negint(e, -8);
        cbor_negint(e, -1);
        cbor_uint(e, 6); /* crv: Ed25519 */
        cbor_negint(e, -2);
        cbor_bytes(e, pub, 32);
    } else if (alg == -257) {
        static const uint8_t exp65537[3] = { 0x01, 0x00, 0x01 };
        cbor_map(e, 4);
        cbor_uint(e, 1);
        cbor_uint(e, 3); /* kty: RSA */
        cbor_uint(e, 3);
        cbor_negint(e, -257);
        cbor_negint(e, -1);
        cbor_bytes(e, pub, 256);
        cbor_negint(e, -2);
        cbor_bytes(e, exp65537, 3);
    } else {
        cbor_map(e, 3);
        cbor_uint(e, 1);
        cbor_uint(e, 7); /* kty: AKP (ML-DSA) */
        cbor_uint(e, 3);
        cbor_negint(e, alg);
        cbor_negint(e, -1);
        cbor_bytes(e, pub, pubLen);
    }
}

static void handle_make_credential(uint32_t channel, const uint8_t *data, size_t len)
{
    uint8_t clientDataHash[32];
    size_t cdhLen = 0;
    char rpId[STORE_RP_ID_MAX] = { 0 };
    uint8_t userId[STORE_USER_ID_MAX];
    size_t userIdLen = 0;
    char userName[STORE_USER_NAME_MAX] = { 0 };
    int alg = 0;
    bool rk = true;
    bool extHmac = false;
    uint8_t extCredProtect = 0;
    bool extLbk = false;
    bool extCredBlob = false;
    uint8_t credBlobBuf[STORE_CREDBLOB_MAX];
    size_t credBlobLen = 0;
    uint8_t pinAuth[32];
    size_t pinAuthLen = 0;
    bool pinAuthPresent = false;
    uint64_t pinProto = 0;

    uint8_t exclIds[16][STORE_CRED_ID_LEN];
    size_t exclCount = 0;

    cbor_dec_t d;
    cbor_dec_init(&d, data + 1, len - 1);

    size_t mapLen;
    if (!cbor_read_map(&d, &mapLen)) {
        send_status(channel, CTAP2_ERR_INVALID_CBOR);
        return;
    }

    for (size_t i = 0; i < mapLen && d.ok; i++) {
        uint64_t key;
        if (!cbor_read_uint(&d, &key)) break;

        switch (key) {
        case 0x01: {
            const uint8_t *p;
            size_t n;
            if (!cbor_read_bytes(&d, &p, &n)) { d.ok = false; break; }
            if (n == 32) {
                memcpy(clientDataHash, p, 32);
                cdhLen = 32;
            }
            break;
        }
        case 0x02: { /* rp */
            size_t m;
            if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
            for (size_t j = 0; j < m && d.ok; j++) {
                char k[32];
                if (!cbor_read_text(&d, k, sizeof(k))) { d.ok = false; break; }
                if (strcmp(k, "id") == 0) {
                    if (!cbor_read_text(&d, rpId, sizeof(rpId))) d.ok = false;
                } else if (!cbor_skip(&d)) {
                    d.ok = false;
                }
            }
            break;
        }
        case 0x03: { /* user */
            size_t m;
            if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
            for (size_t j = 0; j < m && d.ok; j++) {
                char k[32];
                if (!cbor_read_text(&d, k, sizeof(k))) { d.ok = false; break; }
                if (strcmp(k, "id") == 0) {
                    const uint8_t *p;
                    size_t n;
                    if (cbor_read_bytes(&d, &p, &n) && n <= STORE_USER_ID_MAX) {
                        memcpy(userId, p, n);
                        userIdLen = n;
                    } else {
                        d.ok = false;
                    }
                } else if (strcmp(k, "name") == 0) {
                    if (!cbor_read_text(&d, userName, sizeof(userName))) d.ok = false;
                } else if (!cbor_skip(&d)) {
                    d.ok = false;
                }
            }
            break;
        }
        case 0x04: { /* pubKeyCredParams: first algorithm in the platform's order that we support */
            size_t n;
            if (!cbor_read_array(&d, &n)) { d.ok = false; break; }
            bool haveOther = false;
            bool defaultOffered = false;
            for (size_t j = 0; j < n && d.ok; j++) {
                size_t m;
                if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
                for (size_t k = 0; k < m && d.ok; k++) {
                    char kk[32];
                    if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                    if (strcmp(kk, "alg") == 0) {
                        int64_t a;
                        if (!cbor_read_int(&d, &a)) { d.ok = false; break; }
                        if (a == vault_default_alg()) defaultOffered = true;
                        if (alg == 0 && alg_supported(a)) alg = (int)a;
                        else if (alg == 0) haveOther = true;
                    } else if (!cbor_skip(&d)) {
                        d.ok = false;
                    }
                }
            }
            if (defaultOffered && alg_supported(vault_default_alg())) alg = vault_default_alg(); /* configured preference */
            if (alg == 0 && haveOther) alg = 1; /* none supported -> reported below */
            break;
        }
        case 0x05: { /* excludeList */
            size_t n;
            if (!cbor_read_array(&d, &n)) { d.ok = false; break; }
            for (size_t j = 0; j < n && d.ok; j++) {
                size_t m;
                if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
                for (size_t k = 0; k < m && d.ok; k++) {
                    char kk[32];
                    if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                    if (strcmp(kk, "id") == 0) {
                        const uint8_t *p;
                        size_t idLen;
                        if (cbor_read_bytes(&d, &p, &idLen)) {
                            if (exclCount < 16 && idLen == STORE_CRED_ID_LEN) {
                                memcpy(exclIds[exclCount], p, STORE_CRED_ID_LEN);
                                exclCount++;
                            }
                        } else {
                            d.ok = false;
                        }
                    } else if (!cbor_skip(&d)) {
                        d.ok = false;
                    }
                }
            }
            break;
        }
        case 0x06: { /* extensions */
            size_t m;
            if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
            for (size_t j = 0; j < m && d.ok; j++) {
                char kk[32];
                if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                if (strcmp(kk, "hmac-secret") == 0) {
                    if (!cbor_read_bool(&d, &extHmac)) d.ok = false;
                } else if (strcmp(kk, "credProtect") == 0) {
                    uint64_t v;
                    if (!cbor_read_uint(&d, &v) || v < 1 || v > 3) d.ok = false;
                    else extCredProtect = (uint8_t)v;
                } else if (strcmp(kk, "credBlob") == 0) {
                    const uint8_t *p;
                    size_t n;
                    if (!cbor_read_bytes(&d, &p, &n)) d.ok = false;
                    else if (n <= STORE_CREDBLOB_MAX) { memcpy(credBlobBuf, p, n); credBlobLen = n; extCredBlob = true; }
                } else if (strcmp(kk, "largeBlobKey") == 0) {
                    if (!cbor_read_bool(&d, &extLbk)) d.ok = false;
                } else if (!cbor_skip(&d)) {
                    d.ok = false;
                }
            }
            break;
        }
        case 0x08: { /* pinUvAuthParam */
            const uint8_t *p;
            size_t n;
            if (!cbor_read_bytes(&d, &p, &n) || n > sizeof(pinAuth)) { d.ok = false; break; }
            memcpy(pinAuth, p, n);
            pinAuthLen = n;
            pinAuthPresent = true;
            break;
        }
        case 0x09:
            if (!cbor_read_uint(&d, &pinProto)) d.ok = false;
            break;
        case 0x07: { /* options */
            size_t m;
            if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
            for (size_t j = 0; j < m && d.ok; j++) {
                char kk[32];
                if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                if (strcmp(kk, "rk") == 0) {
                    if (!cbor_read_bool(&d, &rk)) d.ok = false;
                } else if (strcmp(kk, "uv") == 0) {
                    bool uv;
                    if (!cbor_read_bool(&d, &uv)) { d.ok = false; break; }
                    if (uv) { /* no user verification yet */
                        send_status(channel, CTAP2_ERR_UNSUPPORTED_OPTION);
                        return;
                    }
                } else if (!cbor_skip(&d)) {
                    d.ok = false;
                }
            }
            break;
        }
        default:
            if (!cbor_skip(&d)) d.ok = false;
            break;
        }
    }

    (void)rk; /* this authenticator is resident-only */

    if (!d.ok) {
        send_status(channel, CTAP2_ERR_INVALID_CBOR);
        return;
    }
    if (cdhLen != 32 || rpId[0] == '\0' || userIdLen == 0) {
        send_status(channel, CTAP2_ERR_MISSING_PARAMETER);
        return;
    }
    if (alg == 0 || alg == 1) {
        send_status(channel, alg == 0 ? CTAP2_ERR_MISSING_PARAMETER : CTAP2_ERR_UNSUPPORTED_ALGORITHM);
        return;
    }

    uint8_t rpIdHash[32];
    sha256((const uint8_t *)rpId, strlen(rpId), rpIdHash);

    /* PIN / UV: a PIN-protected authenticator needs a verified pinUvAuthParam */
    bool uvDone = false;
    if (pinAuthPresent) {
        if (pinAuthLen == 0) { /* "PIN probe" */
            send_status(channel, pin_is_set() ? CTAP2_ERR_PIN_INVALID : CTAP2_ERR_PIN_NOT_SET);
            return;
        }
        uint8_t st = pin_verify_token_auth((uint8_t)pinProto, clientDataHash, 32, pinAuth, pinAuthLen,
                                           PIN_PERM_MC, rpIdHash);
        if (st != CTAP2_OK) {
            send_status(channel, st);
            return;
        }
        uvDone = true;
    } else if (pin_is_set()) {
        send_status(channel, CTAP2_ERR_PUAT_REQUIRED);
        return;
    }

    /* excludeList: refuse if a matching credential exists */
    for (size_t i = 0; i < exclCount; i++) {
        cred_record_t existing;
        if (store_find_by_credid(exclIds[i], &existing) &&
            memcmp(existing.rpIdHash, rpIdHash, 32) == 0) {
            send_status(channel, CTAP2_ERR_CREDENTIAL_EXCLUDED);
            return;
        }
    }

    int up = ctap2_wait_up(channel, 30000);
    if (up != UP_OK) {
        send_status(channel, up == UP_CANCELLED ? CTAP2_ERR_KEEPALIVE_CANCEL : CTAP2_ERR_USER_ACTION_TIMEOUT);
        return;
    }

    cred_record_t rec;
    memset(&rec, 0, sizeof(rec));
    static uint8_t pub[2592];   /* P-256: X||Y, Ed25519: 32, RSA: n (256), ML-DSA: up to 2592 */
    static uint8_t bigKey[STORE_BIG_MAX];  /* wrapped key || public key */
    size_t pubLen = 0;
    size_t bigLen = 0;

    fpga_set_keepalive(channel);

    bool keygenOk = false;
    if (alg == -7) {
        keygenOk = fpga_p256_genkey(rec.blob, pub);
        rec.blobLen = 64;
        pubLen = 64;
    } else if (alg == -8) {
        keygenOk = fpga_ed25519_genkey(rec.blob, pub);
        rec.blobLen = 64;
        pubLen = 32;
    } else if (alg == -257) {
        keygenOk = fpga_rsa_genkey(bigKey, pub);
        bigLen = FPGA_RSA_BLOB_LEN;
        pubLen = 256;
    } else {
        int ps = mldsa_set(alg);
        bigLen = fpga_mldsa_blob_len(ps);
        pubLen = ps == 44 ? 1312 : ps == 65 ? 1952 : 2592;
        keygenOk = bigLen && fpga_mldsa_genkey(ps, bigKey, pub);
    }
    if (keygenOk && bigLen) { /* big slot content: wrapped key || public key (credMgmt needs the latter) */
        rec.blobLen = (uint16_t)bigLen;
        rec.pubLen = (uint16_t)pubLen;
        memcpy(bigKey + bigLen, pub, pubLen);
    }

    if (!keygenOk) {
        fpga_set_keepalive(0);
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }

    ctap2_random(rec.credId, STORE_CRED_ID_LEN);
    rec.credProtect = extCredProtect ? extCredProtect : 1;
    if (extHmac) {
        rec.flags |= STORE_FLAG_HMAC_SECRET;
        ctap2_random(rec.credRandom, sizeof(rec.credRandom));
    }
    memcpy(rec.rpIdHash, rpIdHash, 32);
    memcpy(rec.userId, userId, userIdLen);
    rec.userIdLen = (uint8_t)userIdLen;
    rec.alg = (int16_t)alg;
    rec.signCount = 0;
    strncpy(rec.rpId, rpId, sizeof(rec.rpId) - 1);
    strncpy(rec.userName, userName, sizeof(rec.userName) - 1);

    if (extCredBlob) {
        rec.credBlobLen = (uint8_t)credBlobLen;
        memcpy(rec.credBlob, credBlobBuf, credBlobLen);
    }
    if (extLbk) {
        rec.flags |= STORE_FLAG_LBKEY;
        ctap2_random(rec.lbKey, sizeof(rec.lbKey));
    }

    bool stored = bigLen ? store_add_big(&rec, bigKey, bigLen + pubLen) : store_add(&rec);
    memset(bigKey, 0, sizeof(bigKey));
    if (!stored) {
        fpga_set_keepalive(0);
        send_status(channel, CTAP2_ERR_KEY_STORE_FULL);
        return;
    }

    /* authData: rpIdHash | flags | signCount | attestedCredentialData */
    size_t adLen = 0;
    memcpy(authData, rpIdHash, 32);
    adLen = 32;
    bool haveExt = extHmac || extCredProtect || extCredBlob;
    authData[adLen++] = (uint8_t)(0x41 | (uvDone ? 0x04 : 0) | (haveExt ? 0x80 : 0)); /* UP | AT | UV | ED */
    authData[adLen++] = 0;
    authData[adLen++] = 0;
    authData[adLen++] = 0;
    authData[adLen++] = 0;
    uint8_t aaguid[16];
    get_aaguid(aaguid);
    memcpy(authData + adLen, aaguid, 16);
    adLen += 16;
    authData[adLen++] = 0;
    authData[adLen++] = STORE_CRED_ID_LEN;
    memcpy(authData + adLen, rec.credId, STORE_CRED_ID_LEN);
    adLen += STORE_CRED_ID_LEN;

    cbor_enc_t e;
    cbor_init(&e, authData + adLen, sizeof(authData) - adLen);
    ctap2_put_cose_key(&e, alg, pub, pubLen);
    if (!e.ok) {
        fpga_set_keepalive(0);
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }
    adLen += cbor_len(&e);

    if (haveExt) { /* extension outputs, canonical key order */
        cbor_enc_t x;
        cbor_init(&x, authData + adLen, sizeof(authData) - adLen);
        cbor_map(&x, (extCredBlob ? 1 : 0) + (extCredProtect ? 1 : 0) + (extHmac ? 1 : 0));
        if (extCredBlob) {
            cbor_text(&x, "credBlob");
            cbor_bool(&x, true);
        }
        if (extCredProtect) {
            cbor_text(&x, "credProtect");
            cbor_uint(&x, extCredProtect);
        }
        if (extHmac) {
            cbor_text(&x, "hmac-secret");
            cbor_bool(&x, true);
        }
        if (!x.ok) {
            fpga_set_keepalive(0);
            send_status(channel, CTAP2_ERR_PROCESSING);
            return;
        }
        adLen += cbor_len(&x);
    }

    /* self attestation over authData || clientDataHash */
    memcpy(sign_msg, authData, adLen);
    memcpy(sign_msg + adLen, clientDataHash, 32);

    uint8_t *sig = sig_buf;
    size_t sigLen;
    uint8_t attBlob[64];
    const uint8_t *attCert = NULL;
    size_t attCertLen = 0;
    bool batch = false;
    if (ctap2_attestation(attBlob, &attCert, &attCertLen, &batch) && batch) { /* batch attestation with the provisioned key */
        uint8_t digest[32], rs[64];
        sha256(sign_msg, adLen + 32, digest);
        sigLen = fpga_p256_sign(attBlob, digest, rs) ? der_encode_ecdsa(rs, rs + 32, sig, sizeof(sig_buf)) : 0;
    } else {
        batch = false; /* self attestation */
        sigLen = credential_sign(&rec, sign_msg, adLen + 32, sig, sizeof(sig_buf));
    }
    fpga_set_keepalive(0);

    if (!sigLen) {
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }

    cbor_enc_t r;
    cbor_init(&r, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
    cbor_map(&r, extLbk ? 4 : 3);
    cbor_uint(&r, 1);
    cbor_text(&r, "packed");
    cbor_uint(&r, 2);
    cbor_bytes(&r, authData, adLen);
    cbor_uint(&r, 3);
    cbor_map(&r, batch ? 3 : 2);
    cbor_text(&r, "alg");
    cbor_negint(&r, batch ? -7 : alg);
    cbor_text(&r, "sig");
    cbor_bytes(&r, sig, sigLen);
    if (batch) {
        cbor_text(&r, "x5c");
        cbor_array(&r, 1);
        cbor_bytes(&r, attCert, attCertLen);
    }
    if (extLbk) {
        cbor_uint(&r, 5); /* largeBlobKey */
        cbor_bytes(&r, rec.lbKey, 32);
    }

    if (!r.ok) {
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }

    if (uvDone) pin_token_consume();
    ctap2_resp[0] = CTAP2_OK;
    ctaphid_send(channel, CTAPHID_CBOR, ctap2_resp, (uint16_t)(1 + cbor_len(&r)));
}

#define GA_MAX_MATCHES 8

/* State kept for authenticatorGetNextAssertion */
static struct {
    bool active;
    uint8_t ids[GA_MAX_MATCHES][STORE_CRED_ID_LEN];
    size_t count;
    size_t next;
    uint8_t rpIdHash[32];
    uint8_t cdh[32];
    bool uv;
    bool up;
    bool hmacReq;
    bool credBlobReq;
    bool lbkReq;
    pin_secret_t secret;
    uint8_t saltEnc[80];
    size_t saltEncLen;
} ga;

static bool cred_usable(const cred_record_t *rec, bool haveAllowList, bool uv)
{
    if ((rec->flags & STORE_FLAG_U2F) && !haveAllowList) return false; /* U2F key handles are not discoverable */
    if (rec->credProtect == 3 && !uv) return false;
    if (rec->credProtect == 2 && !haveAllowList && !uv) return false;
    return true;
}

/* Build and send the assertion for ga.ids[idx]; withCount adds numberOfCredentials */
static void emit_assertion(uint32_t channel, size_t idx, bool withCount)
{
    cred_record_t rec;
    if (!store_find_by_credid(ga.ids[idx], &rec)) {
        send_status(channel, CTAP2_ERR_NO_CREDENTIALS);
        return;
    }

    uint32_t signCount = rec.signCount + 1;
    store_update_sign_count(rec.credId, signCount);

    bool hmacOut = ga.hmacReq && (rec.flags & STORE_FLAG_HMAC_SECRET);
    uint8_t extBytes[96];
    size_t extLen = 0;
    if (hmacOut) {
        uint8_t salts[64];
        size_t n = pin_decrypt(&ga.secret, ga.saltEnc, ga.saltEncLen, salts);
        if (n != 32 && n != 64) {
            send_status(channel, CTAP2_ERR_INVALID_PARAMETER);
            return;
        }
        const uint8_t *cr = rec.credRandom + (ga.uv ? 32 : 0);
        uint8_t outs[64];
        for (size_t o = 0; o < n; o += 32) {
            hmac_sha256(cr, 32, salts + o, 32, NULL, 0, outs + o);
        }
        extLen = pin_encrypt(&ga.secret, outs, n, extBytes);
        memset(salts, 0, sizeof(salts));
        memset(outs, 0, sizeof(outs));
    }

    size_t adLen = 0;
    memcpy(authData, ga.rpIdHash, 32);
    adLen = 32;
    bool blobOut = ga.credBlobReq;
    authData[adLen++] = (uint8_t)((ga.up ? 0x01 : 0) | (ga.uv ? 0x04 : 0) | ((hmacOut || blobOut) ? 0x80 : 0));
    authData[adLen++] = (signCount >> 24) & 0xFF;
    authData[adLen++] = (signCount >> 16) & 0xFF;
    authData[adLen++] = (signCount >> 8) & 0xFF;
    authData[adLen++] = signCount & 0xFF;
    if (hmacOut || blobOut) { /* canonical order: credBlob, hmac-secret */
        cbor_enc_t x;
        cbor_init(&x, authData + adLen, sizeof(authData) - adLen);
        cbor_map(&x, (blobOut ? 1 : 0) + (hmacOut ? 1 : 0));
        if (blobOut) {
            cbor_text(&x, "credBlob");
            cbor_bytes(&x, rec.credBlob, rec.credBlobLen);
        }
        if (hmacOut) {
            cbor_text(&x, "hmac-secret");
            cbor_bytes(&x, extBytes, extLen);
        }
        adLen += cbor_len(&x);
    }

    memcpy(sign_msg, authData, adLen);
    memcpy(sign_msg + adLen, ga.cdh, 32);

    fpga_set_keepalive(channel);
    uint8_t *sig = sig_buf;
    size_t sigLen = credential_sign(&rec, sign_msg, adLen + 32, sig, sizeof(sig_buf));
    fpga_set_keepalive(0);

    if (!sigLen) {
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }

    cbor_enc_t r;
    cbor_init(&r, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
    bool lbkOut = ga.lbkReq && (rec.flags & STORE_FLAG_LBKEY);
    cbor_map(&r, 4 + (withCount ? 1 : 0) + (lbkOut ? 1 : 0));
    cbor_uint(&r, 1); /* credential */
    cbor_map(&r, 2);
    cbor_text(&r, "id");
    cbor_bytes(&r, rec.credId, STORE_CRED_ID_LEN);
    cbor_text(&r, "type");
    cbor_text(&r, "public-key");
    cbor_uint(&r, 2); /* authData */
    cbor_bytes(&r, authData, adLen);
    cbor_uint(&r, 3); /* signature */
    cbor_bytes(&r, sig, sigLen);
    cbor_uint(&r, 4); /* user */
    cbor_map(&r, 2);
    cbor_text(&r, "id");
    cbor_bytes(&r, rec.userId, rec.userIdLen);
    cbor_text(&r, "name");
    cbor_text(&r, rec.userName);
    if (withCount) {
        cbor_uint(&r, 5); /* numberOfCredentials */
        cbor_uint(&r, ga.count);
    }
    if (lbkOut) {
        cbor_uint(&r, 7); /* largeBlobKey */
        cbor_bytes(&r, rec.lbKey, 32);
    }

    if (!r.ok) {
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }

    ctap2_resp[0] = CTAP2_OK;
    ctaphid_send(channel, CTAPHID_CBOR, ctap2_resp, (uint16_t)(1 + cbor_len(&r)));
}

static void handle_get_assertion(uint32_t channel, const uint8_t *data, size_t len)
{
    char rpId[STORE_RP_ID_MAX] = { 0 };
    uint8_t clientDataHash[32];
    size_t cdhLen = 0;

    uint8_t allowIds[32][STORE_CRED_ID_LEN];
    size_t allowCount = 0;
    bool haveAllowList = false;

    bool extHmac = false;
    bool extCredBlob = false;
    bool extLbk = false;
    uint8_t hmacPeer[64];
    uint8_t hmacSaltEnc[80], hmacSaltAuth[32];
    size_t hmacSaltEncLen = 0, hmacSaltAuthLen = 0;
    uint64_t hmacProto = 0;
    bool hmacHavePeer = false;

    bool optUp = true;
    uint8_t pinAuth[32];
    size_t pinAuthLen = 0;
    bool pinAuthPresent = false;
    uint64_t pinProto = 0;

    ga.active = false;

    cbor_dec_t d;
    cbor_dec_init(&d, data + 1, len - 1);

    size_t mapLen;
    if (!cbor_read_map(&d, &mapLen)) {
        send_status(channel, CTAP2_ERR_INVALID_CBOR);
        return;
    }

    for (size_t i = 0; i < mapLen && d.ok; i++) {
        uint64_t key;
        if (!cbor_read_uint(&d, &key)) break;

        switch (key) {
        case 0x01:
            if (!cbor_read_text(&d, rpId, sizeof(rpId))) d.ok = false;
            break;

        case 0x02: {
            const uint8_t *p;
            size_t n;
            if (!cbor_read_bytes(&d, &p, &n)) { d.ok = false; break; }
            if (n == 32) {
                memcpy(clientDataHash, p, 32);
                cdhLen = 32;
            }
            break;
        }

        case 0x03: { /* allowList */
            size_t n;
            if (!cbor_read_array(&d, &n)) { d.ok = false; break; }
            haveAllowList = n > 0;
            for (size_t j = 0; j < n && d.ok; j++) {
                size_t m;
                if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
                for (size_t k = 0; k < m && d.ok; k++) {
                    char kk[32];
                    if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                    if (strcmp(kk, "id") == 0) {
                        const uint8_t *p;
                        size_t idLen;
                        if (cbor_read_bytes(&d, &p, &idLen)) {
                            if (allowCount < 32 && idLen == STORE_CRED_ID_LEN) {
                                memcpy(allowIds[allowCount], p, STORE_CRED_ID_LEN);
                                allowCount++;
                            }
                        } else {
                            d.ok = false;
                        }
                    } else if (!cbor_skip(&d)) {
                        d.ok = false;
                    }
                }
            }
            break;
        }

        case 0x04: { /* extensions */
            size_t m;
            if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
            for (size_t j = 0; j < m && d.ok; j++) {
                char kk[32];
                if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                if (strcmp(kk, "credBlob") == 0) {
                    if (!cbor_read_bool(&d, &extCredBlob)) d.ok = false;
                    continue;
                }
                if (strcmp(kk, "largeBlobKey") == 0) {
                    if (!cbor_read_bool(&d, &extLbk)) d.ok = false;
                    continue;
                }
                if (strcmp(kk, "hmac-secret") != 0) {
                    if (!cbor_skip(&d)) d.ok = false;
                    continue;
                }
                size_t hm;
                if (!cbor_read_map(&d, &hm)) { d.ok = false; break; }
                extHmac = true;
                for (size_t q = 0; q < hm && d.ok; q++) {
                    uint64_t hk;
                    if (!cbor_read_uint(&d, &hk)) { d.ok = false; break; }
                    if (hk == 1) {
                        size_t cm;
                        if (!cbor_read_map(&d, &cm)) { d.ok = false; break; }
                        bool gx = false, gy = false;
                        for (size_t c = 0; c < cm && d.ok; c++) {
                            int64_t ck;
                            if (!cbor_read_int(&d, &ck)) { d.ok = false; break; }
                            if (ck == -2 || ck == -3) {
                                const uint8_t *p;
                                size_t n;
                                if (!cbor_read_bytes(&d, &p, &n) || n != 32) { d.ok = false; break; }
                                memcpy(hmacPeer + (ck == -2 ? 0 : 32), p, 32);
                                if (ck == -2) gx = true; else gy = true;
                            } else if (!cbor_skip(&d)) {
                                d.ok = false;
                            }
                        }
                        hmacHavePeer = gx && gy;
                    } else if (hk == 2) {
                        const uint8_t *p;
                        size_t n;
                        if (!cbor_read_bytes(&d, &p, &n) || n > sizeof(hmacSaltEnc)) { d.ok = false; break; }
                        memcpy(hmacSaltEnc, p, n);
                        hmacSaltEncLen = n;
                    } else if (hk == 3) {
                        const uint8_t *p;
                        size_t n;
                        if (!cbor_read_bytes(&d, &p, &n) || n > sizeof(hmacSaltAuth)) { d.ok = false; break; }
                        memcpy(hmacSaltAuth, p, n);
                        hmacSaltAuthLen = n;
                    } else if (hk == 4) {
                        if (!cbor_read_uint(&d, &hmacProto)) d.ok = false;
                    } else if (!cbor_skip(&d)) {
                        d.ok = false;
                    }
                }
            }
            break;
        }

        case 0x05: { /* options */
            size_t m;
            if (!cbor_read_map(&d, &m)) { d.ok = false; break; }
            for (size_t j = 0; j < m && d.ok; j++) {
                char kk[32];
                if (!cbor_read_text(&d, kk, sizeof(kk))) { d.ok = false; break; }
                if (strcmp(kk, "up") == 0) {
                    if (!cbor_read_bool(&d, &optUp)) d.ok = false;
                } else if (strcmp(kk, "uv") == 0) {
                    bool uv;
                    if (!cbor_read_bool(&d, &uv)) { d.ok = false; break; }
                    if (uv) { /* no built-in user verification */
                        send_status(channel, CTAP2_ERR_UNSUPPORTED_OPTION);
                        return;
                    }
                } else if (!cbor_skip(&d)) {
                    d.ok = false;
                }
            }
            break;
        }

        case 0x06: { /* pinUvAuthParam */
            const uint8_t *p;
            size_t n;
            if (!cbor_read_bytes(&d, &p, &n) || n > sizeof(pinAuth)) { d.ok = false; break; }
            memcpy(pinAuth, p, n);
            pinAuthLen = n;
            pinAuthPresent = true;
            break;
        }

        case 0x07:
            if (!cbor_read_uint(&d, &pinProto)) d.ok = false;
            break;

        default:
            if (!cbor_skip(&d)) d.ok = false;
            break;
        }
    }

    if (!d.ok) {
        send_status(channel, CTAP2_ERR_INVALID_CBOR);
        return;
    }
    if (cdhLen != 32 || rpId[0] == '\0') {
        send_status(channel, CTAP2_ERR_MISSING_PARAMETER);
        return;
    }

    uint8_t rpIdHash[32];
    sha256((const uint8_t *)rpId, strlen(rpId), rpIdHash);

    bool uvDone = false;
    if (pinAuthPresent) {
        if (pinAuthLen == 0) {
            send_status(channel, pin_is_set() ? CTAP2_ERR_PIN_INVALID : CTAP2_ERR_PIN_NOT_SET);
            return;
        }
        uint8_t st = pin_verify_token_auth((uint8_t)pinProto, clientDataHash, 32, pinAuth, pinAuthLen,
                                           PIN_PERM_GA, rpIdHash);
        if (st != CTAP2_OK) {
            send_status(channel, st);
            return;
        }
        uvDone = true;
    } else if (pin_is_set() && haveAllowList == false) {
        /* discoverable credentials of a PIN-protected authenticator need the PIN */
        send_status(channel, CTAP2_ERR_PUAT_REQUIRED);
        return;
    }

    /* hmac-secret: ECDH with the platform key, check saltAuth */
    if (extHmac) {
        if (!hmacHavePeer || hmacSaltEncLen == 0 || hmacSaltAuthLen == 0 ||
            !pin_shared_secret((uint8_t)hmacProto, hmacPeer, &ga.secret)) {
            send_status(channel, CTAP2_ERR_MISSING_PARAMETER);
            return;
        }
        if (!pin_secret_auth_ok(&ga.secret, hmacSaltEnc, hmacSaltEncLen, hmacSaltAuth, hmacSaltAuthLen)) {
            memset(&ga.secret, 0, sizeof(ga.secret));
            send_status(channel, CTAP2_ERR_PIN_AUTH_INVALID);
            return;
        }
        memcpy(ga.saltEnc, hmacSaltEnc, hmacSaltEncLen);
        ga.saltEncLen = hmacSaltEncLen;
    }

    /* collect usable credentials */
    ga.count = 0;
    cred_record_t rec;
    if (haveAllowList) {
        for (size_t i = 0; i < allowCount && ga.count < GA_MAX_MATCHES; i++) {
            if (store_find_by_credid(allowIds[i], &rec) && memcmp(rec.rpIdHash, rpIdHash, 32) == 0 &&
                cred_usable(&rec, true, uvDone)) {
                memcpy(ga.ids[ga.count++], rec.credId, STORE_CRED_ID_LEN);
            }
        }
    } else {
        for (int n = 0; ga.count < GA_MAX_MATCHES && store_find_by_rp_nth(rpIdHash, n, &rec); n++) {
            if (cred_usable(&rec, false, uvDone)) {
                memcpy(ga.ids[ga.count++], rec.credId, STORE_CRED_ID_LEN);
            }
        }
    }

    if (ga.count == 0) {
        memset(&ga.secret, 0, sizeof(ga.secret));
        send_status(channel, CTAP2_ERR_NO_CREDENTIALS);
        return;
    }

    if (optUp) {
        int up = ctap2_wait_up(channel, 30000);
        if (up != UP_OK) {
            memset(&ga.secret, 0, sizeof(ga.secret));
            send_status(channel, up == UP_CANCELLED ? CTAP2_ERR_KEEPALIVE_CANCEL : CTAP2_ERR_USER_ACTION_TIMEOUT);
            return;
        }
    }

    memcpy(ga.rpIdHash, rpIdHash, 32);
    memcpy(ga.cdh, clientDataHash, 32);
    ga.uv = uvDone;
    ga.up = optUp;
    ga.hmacReq = extHmac;
    ga.credBlobReq = extCredBlob;
    ga.lbkReq = extLbk;
    ga.next = 1;
    ga.active = ga.count > 1;

    if (uvDone) pin_token_consume();
    emit_assertion(channel, 0, ga.count > 1);
}

static void handle_get_next_assertion(uint32_t channel)
{
    if (!ga.active || ga.next >= ga.count) {
        ga.active = false;
        send_status(channel, CTAP2_ERR_NOT_ALLOWED);
        return;
    }
    size_t idx = ga.next++;
    if (ga.next >= ga.count) ga.active = false;
    emit_assertion(channel, idx, false);
}

static void handle_client_pin(uint32_t channel, const uint8_t *data, size_t len)
{
    size_t n = 0;
    uint8_t st = pin_client_pin(data + 1, len - 1, ctap2_resp + 1, sizeof(ctap2_resp) - 1, &n);
    if (st != CTAP2_OK) {
        send_status(channel, st);
        return;
    }
    ctap2_resp[0] = CTAP2_OK;
    ctaphid_send(channel, CTAPHID_CBOR, ctap2_resp, (uint16_t)(1 + n));
}

static void handle_reset(uint32_t channel)
{
    /* CTAP2: reset is only accepted shortly after power-up (replug the key) */
    if ((xTaskGetTickCount() - boot_tick) > pdMS_TO_TICKS(10000)) {
        send_status(channel, CTAP2_ERR_NOT_ALLOWED);
        return;
    }
    int up = ctap2_wait_up(channel, 10000);
    if (up != UP_OK) {
        send_status(channel, up == UP_CANCELLED ? CTAP2_ERR_KEEPALIVE_CANCEL : CTAP2_ERR_USER_ACTION_TIMEOUT);
        return;
    }
    store_wipe();
    pin_factory_reset();
    ctap2_mgmt_reset();
    ga.active = false;
    send_status(channel, CTAP2_OK);
}

void ctap2_handle(uint32_t channel, const uint8_t *data, size_t len)
{
    if (len == 0) {
        send_status(channel, CTAP2_ERR_INVALID_LENGTH);
        return;
    }

    switch (data[0]) {
    case 0x01:
        handle_make_credential(channel, data, len);
        return;

    case 0x02:
        handle_get_assertion(channel, data, len);
        return;

    case 0x04: {
        cbor_enc_t e;
        cbor_init(&e, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
        build_get_info(&e);
        if (!e.ok) {
            send_status(channel, CTAP2_ERR_PROCESSING);
            return;
        }
        ctap2_resp[0] = CTAP2_OK;
        ctaphid_send(channel, CTAPHID_CBOR, ctap2_resp, (uint16_t)(1 + cbor_len(&e)));
        return;
    }

    case 0x06:
        handle_client_pin(channel, data, len);
        return;

    case 0x07:
        handle_reset(channel);
        return;

    case 0x08:
        handle_get_next_assertion(channel);
        return;

    case 0x0A:
    case 0x41: /* prototype CredentialManagement command id */
        ctap2_credential_management(channel, data, len);
        return;

    case 0x0C:
        ctap2_large_blobs(channel, data, len);
        return;
    default:
        send_status(channel, CTAP2_ERR_INVALID_COMMAND);
        return;
    }
}
