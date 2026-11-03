/*
 * authenticatorCredentialManagement (0x0A) and authenticatorLargeBlobs (0x0C) for the PolKey authenticator
 */
#include <string.h>
#include "ctap2.h"
#include "ctap2_int.h"
#include "pin.h"
#include "sha256.h"
#include "fpga.h"
#include "store.h"

#define CTAP2_ERR_LARGE_BLOB_STORAGE_FULL 0x42
#define CTAP2_ERR_INTEGRITY_FAILURE 0x3D
#define CTAP2_ERR_INVALID_SUBCOMMAND 0x3E

#define send_status ctap2_send_status

/* ---------------- credential management ---------------- */
static struct {
    int rpCount, rpNext;
    uint8_t rpHash[STORE_MAX_CRED][32];
    int rpSlot[STORE_MAX_CRED];       /* a slot holding a credential of that RP (for the rpId text) */
    int credCount, credNext;
    int credSlot[STORE_MAX_CRED];
} cm;

static uint8_t pub_buf[2592];

static bool is_listed(const cred_record_t *rec)
{
    return !(rec->flags & STORE_FLAG_U2F);
}

/* public key of a stored credential; returns its length or 0 */
static size_t credential_public(const cred_record_t *rec, uint8_t *out)
{
    if (rec->alg == -7) return fpga_p256_public(rec->blob, out) ? 64 : 0;
    if (rec->alg == -8) return fpga_ed25519_public(rec->blob, out) ? 32 : 0;
    if (rec->pubLen && store_big_read_pub(rec, out, sizeof(pub_buf))) return rec->pubLen;
    return 0;
}

static void put_user(cbor_enc_t *e, const cred_record_t *rec)
{
    cbor_map(e, 2);
    cbor_text(e, "id");
    cbor_bytes(e, rec->userId, rec->userIdLen);
    cbor_text(e, "name");
    cbor_text(e, rec->userName);
}

static void send_cbor(uint32_t channel, cbor_enc_t *e)
{
    if (!e->ok) {
        send_status(channel, CTAP2_ERR_PROCESSING);
        return;
    }
    ctap2_resp[0] = CTAP2_OK;
    ctaphid_send(channel, CTAPHID_CBOR, ctap2_resp, (uint16_t)(1 + cbor_len(e)));
}

static void emit_rp(uint32_t channel, int idx, bool withTotal)
{
    cred_record_t rec;
    if (store_iter(cm.rpSlot[idx], &rec) != cm.rpSlot[idx]) {
        send_status(channel, CTAP2_ERR_NO_CREDENTIALS);
        return;
    }
    cbor_enc_t e;
    cbor_init(&e, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
    cbor_map(&e, withTotal ? 3 : 2);
    cbor_uint(&e, 3); /* rp */
    cbor_map(&e, 1);
    cbor_text(&e, "id");
    cbor_text(&e, rec.rpId);
    cbor_uint(&e, 4); /* rpIDHash */
    cbor_bytes(&e, cm.rpHash[idx], 32);
    if (withTotal) {
        cbor_uint(&e, 5);
        cbor_uint(&e, cm.rpCount);
    }
    send_cbor(channel, &e);
}

static void emit_credential(uint32_t channel, int idx, bool withTotal)
{
    cred_record_t rec;
    if (store_iter(cm.credSlot[idx], &rec) != cm.credSlot[idx]) {
        send_status(channel, CTAP2_ERR_NO_CREDENTIALS);
        return;
    }
    size_t pl = credential_public(&rec, pub_buf);
    cbor_enc_t e;
    cbor_init(&e, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
    bool lbk = (rec.flags & STORE_FLAG_LBKEY) != 0;
    cbor_map(&e, 3 + (pl ? 1 : 0) + (withTotal ? 1 : 0) + (lbk ? 1 : 0));
    cbor_uint(&e, 6); /* user */
    put_user(&e, &rec);
    cbor_uint(&e, 7); /* credentialID */
    cbor_map(&e, 2);
    cbor_text(&e, "id");
    cbor_bytes(&e, rec.credId, STORE_CRED_ID_LEN);
    cbor_text(&e, "type");
    cbor_text(&e, "public-key");
    if (pl) {
        cbor_uint(&e, 8); /* publicKey */
        ctap2_put_cose_key(&e, rec.alg, pub_buf, pl);
    }
    if (withTotal) {
        cbor_uint(&e, 9);
        cbor_uint(&e, cm.credCount);
    }
    cbor_uint(&e, 10); /* credProtect */
    cbor_uint(&e, rec.credProtect ? rec.credProtect : 1);
    if (lbk) {
        cbor_uint(&e, 11); /* largeBlobKey */
        cbor_bytes(&e, rec.lbKey, 32);
    }
    send_cbor(channel, &e);
}

/* PublicKeyCredentialDescriptor {id, type} -> credential id */
static bool read_descriptor(cbor_dec_t *d, uint8_t id[STORE_CRED_ID_LEN])
{
    size_t m;
    if (!cbor_read_map(d, &m)) return false;
    bool got = false;
    for (size_t i = 0; i < m && d->ok; i++) {
        char k[16];
        if (!cbor_read_text(d, k, sizeof(k))) return false;
        if (strcmp(k, "id") == 0) {
            const uint8_t *p;
            size_t n;
            if (!cbor_read_bytes(d, &p, &n) || n != STORE_CRED_ID_LEN) return false;
            memcpy(id, p, n);
            got = true;
        } else if (!cbor_skip(d)) {
            return false;
        }
    }
    return d->ok && got;
}

void ctap2_credential_management(uint32_t channel, const uint8_t *data, size_t len)
{
    uint64_t sub = 0, proto = 0;
    const uint8_t *paramsRaw = NULL;
    size_t paramsLen = 0;
    const uint8_t *auth = NULL;
    size_t authLen = 0;

    cbor_dec_t d;
    cbor_dec_init(&d, data + 1, len - 1);
    size_t m;
    if (!cbor_read_map(&d, &m)) { send_status(channel, CTAP2_ERR_INVALID_CBOR); return; }
    for (size_t i = 0; i < m && d.ok; i++) {
        uint64_t k;
        if (!cbor_read_uint(&d, &k)) { d.ok = false; break; }
        switch (k) {
        case 1: if (!cbor_read_uint(&d, &sub)) d.ok = false; break;
        case 2: {
            size_t start = d.off;
            if (!cbor_skip(&d)) { d.ok = false; break; }
            paramsRaw = d.buf + start;
            paramsLen = d.off - start;
            break;
        }
        case 3: if (!cbor_read_uint(&d, &proto)) d.ok = false; break;
        case 4: if (!cbor_read_bytes(&d, &auth, &authLen)) d.ok = false; break;
        default: if (!cbor_skip(&d)) d.ok = false; break;
        }
    }
    if (!d.ok) { send_status(channel, CTAP2_ERR_INVALID_CBOR); return; }
    if (sub == 0) { send_status(channel, CTAP2_ERR_MISSING_PARAMETER); return; }

    /* subcommand parameters */
    uint8_t rpHash[32];
    bool haveRpHash = false;
    uint8_t credId[STORE_CRED_ID_LEN];
    bool haveCred = false;
    uint8_t userId[STORE_USER_ID_MAX];
    size_t userIdLen = 0;
    char userName[STORE_USER_NAME_MAX] = { 0 };
    bool haveUser = false;
    if (paramsRaw) {
        cbor_dec_t p;
        cbor_dec_init(&p, paramsRaw, paramsLen);
        size_t pm;
        if (!cbor_read_map(&p, &pm)) { send_status(channel, CTAP2_ERR_INVALID_CBOR); return; }
        for (size_t i = 0; i < pm && p.ok; i++) {
            uint64_t k;
            if (!cbor_read_uint(&p, &k)) { p.ok = false; break; }
            if (k == 1) {
                const uint8_t *b;
                size_t n;
                if (!cbor_read_bytes(&p, &b, &n) || n != 32) { p.ok = false; break; }
                memcpy(rpHash, b, 32);
                haveRpHash = true;
            } else if (k == 2) {
                haveCred = read_descriptor(&p, credId);
                if (!haveCred) p.ok = false;
            } else if (k == 3) {
                size_t um;
                if (!cbor_read_map(&p, &um)) { p.ok = false; break; }
                for (size_t j = 0; j < um && p.ok; j++) {
                    char kk[16];
                    if (!cbor_read_text(&p, kk, sizeof(kk))) { p.ok = false; break; }
                    if (strcmp(kk, "id") == 0) {
                        const uint8_t *b;
                        size_t n;
                        if (!cbor_read_bytes(&p, &b, &n) || n > STORE_USER_ID_MAX) { p.ok = false; break; }
                        memcpy(userId, b, n);
                        userIdLen = n;
                        haveUser = true;
                    } else if (strcmp(kk, "name") == 0) {
                        if (!cbor_read_text(&p, userName, sizeof(userName))) p.ok = false;
                    } else if (!cbor_skip(&p)) {
                        p.ok = false;
                    }
                }
            } else if (!cbor_skip(&p)) {
                p.ok = false;
            }
        }
        if (!p.ok) { send_status(channel, CTAP2_ERR_INVALID_CBOR); return; }
    }

    /* the *GetNext* subcommands carry no authentication */
    if (sub != 3 && sub != 5) {
        if (sub < 1 || sub > 7) { send_status(channel, CTAP2_ERR_INVALID_SUBCOMMAND); return; }
        if (!auth) { send_status(channel, pin_is_set() ? CTAP2_ERR_PUAT_REQUIRED : CTAP2_ERR_PIN_NOT_SET); return; }
        uint8_t msg[8 + 160];
        if (paramsLen > 160) { send_status(channel, CTAP2_ERR_INVALID_LENGTH); return; }
        msg[0] = (uint8_t)sub;
        if (paramsLen) memcpy(msg + 1, paramsRaw, paramsLen);

        uint8_t bind[32] = { 0 };
        if (haveRpHash) memcpy(bind, rpHash, 32);
        else if (haveCred) {
            cred_record_t r;
            if (store_find_by_credid(credId, &r)) memcpy(bind, r.rpIdHash, 32);
        }
        uint8_t st = pin_verify_token_auth((uint8_t)proto, msg, 1 + paramsLen, auth, authLen, PIN_PERM_CM, bind);
        if (st != CTAP2_OK) { send_status(channel, st); return; }
    }

    switch (sub) {
    case 1: { /* getCredsMetadata */
        int n = 0;
        cred_record_t rec;
        for (int i = store_iter(0, &rec); i >= 0; i = store_iter(i + 1, &rec)) {
            if (is_listed(&rec)) n++;
        }
        cbor_enc_t e;
        cbor_init(&e, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
        cbor_map(&e, 2);
        cbor_uint(&e, 1);
        cbor_uint(&e, (uint64_t)n);
        cbor_uint(&e, 2);
        cbor_uint(&e, (uint64_t)(store_capacity() - store_count()));
        send_cbor(channel, &e);
        return;
    }

    case 2: { /* enumerateRPsBegin */
        cm.rpCount = 0;
        cm.rpNext = 0;
        cred_record_t rec;
        for (int i = store_iter(0, &rec); i >= 0; i = store_iter(i + 1, &rec)) {
            if (!is_listed(&rec)) continue;
            bool seen = false;
            for (int j = 0; j < cm.rpCount; j++) {
                if (memcmp(cm.rpHash[j], rec.rpIdHash, 32) == 0) { seen = true; break; }
            }
            if (!seen) {
                memcpy(cm.rpHash[cm.rpCount], rec.rpIdHash, 32);
                cm.rpSlot[cm.rpCount++] = i;
            }
        }
        if (cm.rpCount == 0) { send_status(channel, CTAP2_ERR_NO_CREDENTIALS); return; }
        cm.rpNext = 1;
        emit_rp(channel, 0, true);
        return;
    }

    case 3: /* enumerateRPsGetNextRP */
        if (cm.rpNext <= 0 || cm.rpNext >= cm.rpCount) { send_status(channel, CTAP2_ERR_NOT_ALLOWED); return; }
        emit_rp(channel, cm.rpNext++, false);
        return;

    case 4: { /* enumerateCredentialsBegin */
        if (!haveRpHash) { send_status(channel, CTAP2_ERR_MISSING_PARAMETER); return; }
        cm.credCount = 0;
        cm.credNext = 0;
        cred_record_t rec;
        for (int i = store_iter(0, &rec); i >= 0; i = store_iter(i + 1, &rec)) {
            if (is_listed(&rec) && memcmp(rec.rpIdHash, rpHash, 32) == 0) cm.credSlot[cm.credCount++] = i;
        }
        if (cm.credCount == 0) { send_status(channel, CTAP2_ERR_NO_CREDENTIALS); return; }
        cm.credNext = 1;
        emit_credential(channel, 0, true);
        return;
    }

    case 5: /* enumerateCredentialsGetNextCredential */
        if (cm.credNext <= 0 || cm.credNext >= cm.credCount) { send_status(channel, CTAP2_ERR_NOT_ALLOWED); return; }
        emit_credential(channel, cm.credNext++, false);
        return;

    case 6: /* deleteCredential */
        if (!haveCred) { send_status(channel, CTAP2_ERR_MISSING_PARAMETER); return; }
        cm.rpNext = cm.credNext = 0;
        send_status(channel, store_delete_by_credid(credId) ? CTAP2_OK : CTAP2_ERR_NO_CREDENTIALS);
        return;

    case 7: /* updateUserInformation */
        if (!haveCred || !haveUser) { send_status(channel, CTAP2_ERR_MISSING_PARAMETER); return; }
        send_status(channel, store_update_user(credId, userId, userIdLen, userName) ? CTAP2_OK : CTAP2_ERR_NO_CREDENTIALS);
        return;
    }
}

/* ---------------- large blobs ---------------- */
static uint8_t lb_buf[STORE_LB_MAX + 32];
static size_t lb_expected, lb_received;
static bool lb_writing;

/* serialized empty array: 0x80 || LEFT(SHA-256(0x80), 16) */
static size_t lb_load(uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (store_lb_read(out, cap, &n)) return n;
    static const uint8_t empty = 0x80;
    uint8_t h[32];
    ctap2_sha256(&empty, 1, h);
    out[0] = 0x80;
    memcpy(out + 1, h, 16);
    return 17;
}

void ctap2_mgmt_reset(void)
{
    cm.rpCount = cm.rpNext = cm.credCount = cm.credNext = 0;
    lb_writing = false;
}

void ctap2_large_blobs(uint32_t channel, const uint8_t *data, size_t len)
{
    uint64_t get = 0, offset = 0, length = 0, proto = 0;
    bool haveGet = false, haveSet = false, haveOffset = false, haveLength = false;
    const uint8_t *set = NULL;
    size_t setLen = 0;
    const uint8_t *auth = NULL;
    size_t authLen = 0;

    cbor_dec_t d;
    cbor_dec_init(&d, data + 1, len - 1);
    size_t m;
    if (!cbor_read_map(&d, &m)) { send_status(channel, CTAP2_ERR_INVALID_CBOR); return; }
    for (size_t i = 0; i < m && d.ok; i++) {
        uint64_t k;
        if (!cbor_read_uint(&d, &k)) { d.ok = false; break; }
        switch (k) {
        case 1: haveGet = cbor_read_uint(&d, &get); if (!haveGet) d.ok = false; break;
        case 2: haveSet = cbor_read_bytes(&d, &set, &setLen); if (!haveSet) d.ok = false; break;
        case 3: haveOffset = cbor_read_uint(&d, &offset); if (!haveOffset) d.ok = false; break;
        case 4: haveLength = cbor_read_uint(&d, &length); if (!haveLength) d.ok = false; break;
        case 5: if (!cbor_read_bytes(&d, &auth, &authLen)) d.ok = false; break;
        case 6: if (!cbor_read_uint(&d, &proto)) d.ok = false; break;
        default: if (!cbor_skip(&d)) d.ok = false; break;
        }
    }
    if (!d.ok) { send_status(channel, CTAP2_ERR_INVALID_CBOR); return; }
    if (!haveOffset || haveGet == haveSet) { send_status(channel, CTAP2_ERR_MISSING_PARAMETER); return; }

    if (haveGet) {
        static uint8_t cur[STORE_LB_MAX];
        size_t n = lb_load(cur, sizeof(cur));
        if (offset > n) { send_status(channel, CTAP2_ERR_INVALID_PARAMETER); return; }
        size_t take = n - offset < get ? n - (size_t)offset : (size_t)get;
        cbor_enc_t e;
        cbor_init(&e, ctap2_resp + 1, sizeof(ctap2_resp) - 1);
        cbor_map(&e, 1);
        cbor_uint(&e, 1);
        cbor_bytes(&e, cur + offset, take);
        send_cbor(channel, &e);
        return;
    }

    /* set */
    if (offset == 0) {
        if (!haveLength) { send_status(channel, CTAP2_ERR_MISSING_PARAMETER); return; }
        if (length > STORE_LB_MAX) { send_status(channel, CTAP2_ERR_LARGE_BLOB_STORAGE_FULL); return; }
        if (length < 17) { send_status(channel, CTAP2_ERR_INVALID_PARAMETER); return; }
        lb_expected = (size_t)length;
        lb_received = 0;
        lb_writing = true;
    } else {
        if (haveLength) { send_status(channel, CTAP2_ERR_INVALID_PARAMETER); return; }
        if (!lb_writing || offset != lb_received) { send_status(channel, CTAP2_ERR_INVALID_SEQ); return; }
    }

    if (pin_is_set() || auth) {
        if (!auth) { send_status(channel, CTAP2_ERR_PUAT_REQUIRED); return; }
        uint8_t msg[32 + 2 + 4 + 32];
        memset(msg, 0xFF, 32);
        msg[32] = 0x0C;
        msg[33] = 0x00;
        msg[34] = (uint8_t)offset;
        msg[35] = (uint8_t)(offset >> 8);
        msg[36] = (uint8_t)(offset >> 16);
        msg[37] = (uint8_t)(offset >> 24);
        ctap2_sha256(set, setLen, msg + 38);
        uint8_t zero[32] = { 0 };
        uint8_t st = pin_verify_token_auth((uint8_t)proto, msg, sizeof(msg), auth, authLen, PIN_PERM_LBW, zero);
        if (st != CTAP2_OK) { lb_writing = false; send_status(channel, st); return; }
    }

    if (lb_received + setLen > lb_expected) { lb_writing = false; send_status(channel, CTAP2_ERR_INVALID_PARAMETER); return; }
    memcpy(lb_buf + lb_received, set, setLen);
    lb_received += setLen;

    if (lb_received == lb_expected) {
        lb_writing = false;
        uint8_t h[32];
        ctap2_sha256(lb_buf, lb_expected - 16, h);
        if (memcmp(h, lb_buf + lb_expected - 16, 16) != 0) { send_status(channel, CTAP2_ERR_INTEGRITY_FAILURE); return; }
        if (!store_lb_write(lb_buf, lb_expected)) { send_status(channel, CTAP2_ERR_PROCESSING); return; }
    }
    send_status(channel, CTAP2_OK);
}
