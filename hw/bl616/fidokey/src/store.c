/*
 * Credential store for the PolKey FIDO2 authenticator (BL616)
 */
#include <string.h>
#include "bflb_flash.h"
#include "store.h"

#define SLOT_SIZE 1024u
#define SLOTS_PER_SECTOR 4u
#define SECTOR_SIZE 4096u
#define STORE_MAGIC 0x474B4352u /* "GKCR" */
#define STORE_VERSION 3u

/* Serialized slot layout (little-endian):
 *   0   magic u32 | version u16 | blobLen u16
 *   8   rpIdHash[32]
 *   40  credId[16]
 *   56  userId[64] | userIdLen u8 | alg i8 | pad[2]
 *   124 signCount u32
 *   128 blob[64]
 *   192 rpId[128]
 *   320 userName[64]
 */
#define OFF_MAGIC 0
#define OFF_VERSION 4
#define OFF_BLOBLEN 6
#define OFF_RPIDHASH 8
#define OFF_CREDID 40
#define OFF_USERID 56
#define OFF_USERIDLEN 120
#define OFF_ALG 121
#define OFF_SIGNCOUNT 124
#define OFF_BLOB 128
#define OFF_RPID 192
#define OFF_USERNAME 320
#define OFF_CREDRANDOM 384
#define OFF_FLAGS 448
#define OFF_CREDPROTECT 449
#define OFF_BIGSLOT 450
#define OFF_PUBLEN 452
#define OFF_CREDBLOBLEN 454
#define OFF_CREDBLOB 456
#define OFF_LBKEY 488

#define BIG_SLOTS 16u
#define BIG_SLOT_SIZE 8192u /* two sectors */

static uint32_t region_addr;
static uint32_t meta_addr(void);
static int cred_count;
static bool store_ready;
static uint8_t sector_buf[SECTOR_SIZE];

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

static bool slot_read(int idx, uint8_t *slot)
{
    return bflb_flash_read(region_addr + (uint32_t)idx * SLOT_SIZE, slot, SLOT_SIZE) == 0;
}

static bool slot_valid(const uint8_t *slot)
{
    return get_u32(slot + OFF_MAGIC) == STORE_MAGIC && get_u16(slot + OFF_VERSION) == STORE_VERSION;
}

static void slot_deserialize(const uint8_t *slot, cred_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    memcpy(rec->rpIdHash, slot + OFF_RPIDHASH, 32);
    memcpy(rec->credId, slot + OFF_CREDID, STORE_CRED_ID_LEN);
    memcpy(rec->userId, slot + OFF_USERID, STORE_USER_ID_MAX);
    rec->userIdLen = slot[OFF_USERIDLEN];
    rec->alg = (int16_t)get_u16(slot + OFF_ALG);
    rec->signCount = get_u32(slot + OFF_SIGNCOUNT);
    rec->blobLen = get_u16(slot + OFF_BLOBLEN);
    memcpy(rec->blob, slot + OFF_BLOB, STORE_BLOB_MAX);
    memcpy(rec->rpId, slot + OFF_RPID, STORE_RP_ID_MAX);
    memcpy(rec->userName, slot + OFF_USERNAME, STORE_USER_NAME_MAX);
    memcpy(rec->credRandom, slot + OFF_CREDRANDOM, 64);
    rec->flags = slot[OFF_FLAGS];
    rec->credProtect = slot[OFF_CREDPROTECT];
    rec->bigSlot = slot[OFF_BIGSLOT];
    rec->pubLen = get_u16(slot + OFF_PUBLEN);
    rec->credBlobLen = slot[OFF_CREDBLOBLEN] <= STORE_CREDBLOB_MAX ? slot[OFF_CREDBLOBLEN] : 0;
    memcpy(rec->credBlob, slot + OFF_CREDBLOB, STORE_CREDBLOB_MAX);
    memcpy(rec->lbKey, slot + OFF_LBKEY, 32);
    rec->rpId[STORE_RP_ID_MAX - 1] = '\0';
    rec->userName[STORE_USER_NAME_MAX - 1] = '\0';
}

static void slot_serialize(uint8_t *slot, const cred_record_t *rec)
{
    memset(slot, 0, SLOT_SIZE);
    put_u32(slot + OFF_MAGIC, STORE_MAGIC);
    put_u16(slot + OFF_VERSION, STORE_VERSION);
    put_u16(slot + OFF_BLOBLEN, rec->blobLen);
    memcpy(slot + OFF_RPIDHASH, rec->rpIdHash, 32);
    memcpy(slot + OFF_CREDID, rec->credId, STORE_CRED_ID_LEN);
    memcpy(slot + OFF_USERID, rec->userId, STORE_USER_ID_MAX);
    slot[OFF_USERIDLEN] = rec->userIdLen;
    put_u16(slot + OFF_ALG, (uint16_t)rec->alg);
    put_u32(slot + OFF_SIGNCOUNT, rec->signCount);
    memcpy(slot + OFF_BLOB, rec->blob, STORE_BLOB_MAX);
    memcpy(slot + OFF_CREDRANDOM, rec->credRandom, 64);
    slot[OFF_FLAGS] = rec->flags;
    slot[OFF_CREDPROTECT] = rec->credProtect;
    slot[OFF_BIGSLOT] = rec->bigSlot;
    put_u16(slot + OFF_PUBLEN, rec->pubLen);
    slot[OFF_CREDBLOBLEN] = rec->credBlobLen;
    memcpy(slot + OFF_CREDBLOB, rec->credBlob, STORE_CREDBLOB_MAX);
    memcpy(slot + OFF_LBKEY, rec->lbKey, 32);
    strncpy((char *)slot + OFF_RPID, rec->rpId, STORE_RP_ID_MAX - 1);
    strncpy((char *)slot + OFF_USERNAME, rec->userName, STORE_USER_NAME_MAX - 1);
}

/* Rewrite the whole 4 KB sector holding the slot (erase + program). */
static bool slot_write(int idx, const cred_record_t *rec)
{
    uint32_t sector = region_addr + ((uint32_t)idx / SLOTS_PER_SECTOR) * SECTOR_SIZE;
    size_t slot_off = ((uint32_t)idx % SLOTS_PER_SECTOR) * SLOT_SIZE;

    if (bflb_flash_read(sector, sector_buf, SECTOR_SIZE) != 0) {
        memset(sector_buf, 0xFF, SECTOR_SIZE);
    }

    slot_serialize(sector_buf + slot_off, rec);

    if (bflb_flash_erase(sector, SECTOR_SIZE) != 0) return false;
    return bflb_flash_write(sector, sector_buf, SECTOR_SIZE) == 0;
}

bool store_init(void)
{
    uint32_t sz = bflb_flash_get_size();
    if (sz < 0x100000) sz = 0x400000;
    region_addr = sz - 0x10000;

    cred_count = 0;
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (slot_read(i, slot) && slot_valid(slot)) {
            cred_count++;
        }
    }
    store_ready = true;
    return true;
}

int store_count(void)
{
    return cred_count;
}

static int find_slot(const uint8_t credId[STORE_CRED_ID_LEN])
{
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (slot_read(i, slot) && slot_valid(slot) && memcmp(slot + OFF_CREDID, credId, STORE_CRED_ID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* flash layout (growing down from the end): credentials 64 KB | meta 4 KB | U2F attestation 4 KB |
   large blob 8 KB | big slots 16 x 8 KB */
#define LB_SIZE 8192u
static uint32_t att_addr(void) { return meta_addr() - SECTOR_SIZE; }
static uint32_t lb_addr(void) { return att_addr() - LB_SIZE; }

static uint32_t big_addr(unsigned n)
{
    return lb_addr() - BIG_SLOTS * BIG_SLOT_SIZE + n * BIG_SLOT_SIZE;
}

/* vault: STORE_VAULT_SECTORS sectors right below the big slots */
static uint32_t vault_addr(void)
{
    return lb_addr() - BIG_SLOTS * BIG_SLOT_SIZE - STORE_VAULT_SECTORS * SECTOR_SIZE;
}

bool store_vault_read(uint32_t off, uint8_t *out, size_t n)
{
    if (!store_ready || off + n > STORE_VAULT_SECTORS * SECTOR_SIZE) return false;
    return bflb_flash_read(vault_addr() + off, out, n) == 0;
}

bool store_vault_write_sector(unsigned sector, const uint8_t *data)
{
    if (!store_ready || sector >= STORE_VAULT_SECTORS) return false;
    uint32_t a = vault_addr() + sector * SECTOR_SIZE;
    if (bflb_flash_erase(a, SECTOR_SIZE) != 0) return false;
    return bflb_flash_write(a, (uint8_t *)data, SECTOR_SIZE) == 0;
}

void store_vault_erase(void)
{
    if (!store_ready) return;
    for (unsigned i = 0; i < STORE_VAULT_SECTORS; i++) bflb_flash_erase(vault_addr() + i * SECTOR_SIZE, SECTOR_SIZE);
}

/* Bitmap of big slots referenced by valid credentials */
static uint32_t big_used_mask(void)
{
    uint32_t used = 0;
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (slot_read(i, slot) && slot_valid(slot) && slot[OFF_BIGSLOT] && slot[OFF_BIGSLOT] <= BIG_SLOTS) {
            used |= 1u << (slot[OFF_BIGSLOT] - 1);
        }
    }
    return used;
}

static bool big_write(unsigned n, const uint8_t *data, size_t len)
{
    uint32_t a = big_addr(n);
    if (bflb_flash_erase(a, BIG_SLOT_SIZE) != 0) return false;
    uint8_t chunk[256];
    for (size_t o = 0; o < len; o += sizeof(chunk)) {
        size_t n2 = len - o < sizeof(chunk) ? len - o : sizeof(chunk);
        memset(chunk, 0xFF, sizeof(chunk));
        memcpy(chunk, data + o, n2);
        if (bflb_flash_write(a + (uint32_t)o, chunk, (n2 + 3) & ~3u) != 0) return false;
    }
    return true;
}

bool store_big_read(const cred_record_t *rec, uint8_t *out, size_t cap)
{
    if (rec->bigSlot == 0 || rec->bigSlot > BIG_SLOTS || rec->blobLen > cap) return false;
    return bflb_flash_read(big_addr(rec->bigSlot - 1u), out, rec->blobLen) == 0;
}

bool store_big_read_pub(const cred_record_t *rec, uint8_t *out, size_t cap)
{
    if (rec->bigSlot == 0 || rec->bigSlot > BIG_SLOTS || rec->pubLen == 0 || rec->pubLen > cap ||
        (size_t)rec->blobLen + rec->pubLen > BIG_SLOT_SIZE) return false;
    return bflb_flash_read(big_addr(rec->bigSlot - 1u) + rec->blobLen, out, rec->pubLen) == 0;
}

bool store_add_big(cred_record_t *recIn, const uint8_t *big, size_t bigLen)
{
    if (!store_ready) return false;
    if (big && (bigLen == 0 || bigLen > STORE_BIG_MAX || bigLen > BIG_SLOT_SIZE)) return false;

    cred_record_t rec = *recIn;
    int target = -1;
    uint8_t oldBig = 0;

    /* Replace an existing credential for the same rp + user (re-registration). */
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (!slot_read(i, slot) || !slot_valid(slot)) continue;
        if (memcmp(slot + OFF_RPIDHASH, rec.rpIdHash, 32) == 0 &&
            slot[OFF_USERIDLEN] == rec.userIdLen &&
            memcmp(slot + OFF_USERID, rec.userId, rec.userIdLen) == 0) {
            target = i;
            oldBig = slot[OFF_BIGSLOT];
            break;
        }
    }

    bool isNew = false;
    if (target < 0) {
        for (int i = 0; i < STORE_MAX_CRED; i++) {
            uint8_t slot[SLOT_SIZE];
            if (slot_read(i, slot) && !slot_valid(slot)) {
                target = i;
                isNew = true;
                break;
            }
        }
        if (target < 0) return false; /* full */
    }

    rec.bigSlot = 0;
    if (big) {
        unsigned n;
        if (oldBig && oldBig <= BIG_SLOTS) {
            n = oldBig - 1u;
        } else {
            uint32_t used = big_used_mask();
            for (n = 0; n < BIG_SLOTS && (used & (1u << n)); n++) {}
            if (n == BIG_SLOTS) return false; /* no big slot left */
        }
        if (!big_write(n, big, bigLen)) return false;
        rec.bigSlot = (uint8_t)(n + 1u);
    }

    if (!slot_write(target, &rec)) return false;
    if (isNew) cred_count++;
    recIn->bigSlot = rec.bigSlot;
    return true;
}

bool store_add(const cred_record_t *rec)
{
    cred_record_t copy = *rec;
    return store_add_big(&copy, NULL, 0);
}

bool store_find_by_credid(const uint8_t id[STORE_CRED_ID_LEN], cred_record_t *out)
{
    int i = find_slot(id);
    if (i < 0) return false;
    uint8_t slot[SLOT_SIZE];
    if (!slot_read(i, slot)) return false;
    slot_deserialize(slot, out);
    return true;
}

bool store_find_by_rp(const uint8_t rpIdHash[32], cred_record_t *out)
{
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (!slot_read(i, slot) || !slot_valid(slot)) continue;
        if (memcmp(slot + OFF_RPIDHASH, rpIdHash, 32) == 0) {
            slot_deserialize(slot, out);
            return true;
        }
    }
    return false;
}

int store_count_by_rp(const uint8_t rpIdHash[32])
{
    int n = 0;
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (slot_read(i, slot) && slot_valid(slot) && memcmp(slot + OFF_RPIDHASH, rpIdHash, 32) == 0) n++;
    }
    return n;
}

bool store_find_by_rp_nth(const uint8_t rpIdHash[32], int n, cred_record_t *out)
{
    for (int i = 0; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (!slot_read(i, slot) || !slot_valid(slot)) continue;
        if (memcmp(slot + OFF_RPIDHASH, rpIdHash, 32) == 0 && n-- == 0) {
            slot_deserialize(slot, out);
            return true;
        }
    }
    return false;
}

bool store_update_sign_count(const uint8_t credId[STORE_CRED_ID_LEN], uint32_t signCount)
{
    int i = find_slot(credId);
    if (i < 0) return false;

    uint8_t slot[SLOT_SIZE];
    if (!slot_read(i, slot)) return false;
    put_u32(slot + OFF_SIGNCOUNT, signCount);

    uint32_t sector = region_addr + ((uint32_t)i / SLOTS_PER_SECTOR) * SECTOR_SIZE;
    size_t slot_off = ((uint32_t)i % SLOTS_PER_SECTOR) * SLOT_SIZE;
    if (bflb_flash_read(sector, sector_buf, SECTOR_SIZE) != 0) return false;
    memcpy(sector_buf + slot_off + OFF_SIGNCOUNT, slot + OFF_SIGNCOUNT, 4);
    if (bflb_flash_erase(sector, SECTOR_SIZE) != 0) return false;
    return bflb_flash_write(sector, sector_buf, SECTOR_SIZE) == 0;
}


/* Factory reset: all credentials and the PIN */
void store_wipe(void)
{
    if (!store_ready) return;
    bflb_flash_erase(meta_addr(), SECTOR_SIZE);
    bflb_flash_erase(att_addr(), SECTOR_SIZE);
    bflb_flash_erase(lb_addr(), LB_SIZE);
    for (unsigned n = 0; n < BIG_SLOTS; n++) bflb_flash_erase(big_addr(n), BIG_SLOT_SIZE);
    for (int s = 0; s < (int)(STORE_MAX_CRED / SLOTS_PER_SECTOR); s++) {
        bflb_flash_erase(region_addr + (uint32_t)s * SECTOR_SIZE, SECTOR_SIZE);
    }
    cred_count = 0;
}

/* ---------------- metadata sector ---------------- */
#define META_MAGIC 0x474B4D54u /* "GKMT" */
#define META_RETRIES_DEFAULT 8

static uint32_t meta_addr(void)
{
    return region_addr - SECTOR_SIZE;
}

bool store_meta_load(store_meta_t *m)
{
    uint8_t b[64];
    memset(m, 0, sizeof(*m));
    m->retries = META_RETRIES_DEFAULT;
    if (!store_ready || bflb_flash_read(meta_addr(), b, sizeof(b)) != 0) return false;
    if (get_u32(b) != META_MAGIC) return false;
    m->pinSet = b[4];
    m->retries = b[5];
    memcpy(m->pinSalt, b + 8, 16);
    memcpy(m->pinVerifier, b + 24, 32);
    return true;
}

bool store_meta_save(const store_meta_t *m)
{
    if (!store_ready) return false;
    uint8_t b[64];
    memset(b, 0xFF, sizeof(b));
    put_u32(b, META_MAGIC);
    b[4] = m->pinSet;
    b[5] = m->retries;
    memcpy(b + 8, m->pinSalt, 16);
    memcpy(b + 24, m->pinVerifier, 32);
    if (bflb_flash_erase(meta_addr(), SECTOR_SIZE) != 0) return false;
    return bflb_flash_write(meta_addr(), b, sizeof(b)) == 0;
}

/* ---------------- iteration / delete / update ---------------- */
int store_capacity(void)
{
    return STORE_MAX_CRED;
}

int store_iter(int idx, cred_record_t *out)
{
    for (int i = idx < 0 ? 0 : idx; i < STORE_MAX_CRED; i++) {
        uint8_t slot[SLOT_SIZE];
        if (slot_read(i, slot) && slot_valid(slot)) {
            if (out) slot_deserialize(slot, out);
            return i;
        }
    }
    return -1;
}

bool store_delete_by_credid(const uint8_t id[STORE_CRED_ID_LEN])
{
    int i = find_slot(id);
    if (i < 0) return false;
    uint32_t sector = region_addr + ((uint32_t)i / SLOTS_PER_SECTOR) * SECTOR_SIZE;
    size_t off = ((uint32_t)i % SLOTS_PER_SECTOR) * SLOT_SIZE;
    if (bflb_flash_read(sector, sector_buf, SECTOR_SIZE) != 0) return false;
    memset(sector_buf + off, 0xFF, SLOT_SIZE);
    if (bflb_flash_erase(sector, SECTOR_SIZE) != 0) return false;
    if (bflb_flash_write(sector, sector_buf, SECTOR_SIZE) != 0) return false;
    if (cred_count > 0) cred_count--;
    return true;
}

bool store_update_user(const uint8_t id[STORE_CRED_ID_LEN], const uint8_t *userId, size_t userIdLen, const char *userName)
{
    int i = find_slot(id);
    if (i < 0) return false;
    uint8_t slot[SLOT_SIZE];
    cred_record_t rec;
    if (!slot_read(i, slot)) return false;
    slot_deserialize(slot, &rec);
    if (userIdLen != rec.userIdLen || memcmp(userId, rec.userId, userIdLen) != 0) return false;
    memset(rec.userName, 0, sizeof(rec.userName));
    strncpy(rec.userName, userName, sizeof(rec.userName) - 1);
    return slot_write(i, &rec);
}

/* ---------------- large blob array ---------------- */
#define LB_MAGIC 0x474B4C42u /* "GKLB" */

bool store_lb_read(uint8_t *out, size_t cap, size_t *len)
{
    uint8_t h[8];
    if (!store_ready || bflb_flash_read(lb_addr(), h, sizeof(h)) != 0) return false;
    if (get_u32(h) != LB_MAGIC) return false;
    size_t n = get_u16(h + 4);
    if (n > STORE_LB_MAX || n > cap) return false;
    if (n && bflb_flash_read(lb_addr() + 8, out, n) != 0) return false;
    *len = n;
    return true;
}

bool store_lb_write(const uint8_t *data, size_t len)
{
    if (!store_ready || len > STORE_LB_MAX) return false;
    if (bflb_flash_erase(lb_addr(), LB_SIZE) != 0) return false;
    uint8_t chunk[256];
    memset(chunk, 0xFF, 8);
    put_u32(chunk, LB_MAGIC);
    put_u16(chunk + 4, (uint16_t)len);
    size_t first = len < sizeof(chunk) - 8 ? len : sizeof(chunk) - 8;
    memcpy(chunk + 8, data, first);
    if (bflb_flash_write(lb_addr(), chunk, (8 + first + 3) & ~3u) != 0) return false;
    for (size_t o = first; o < len; o += sizeof(chunk)) {
        size_t n = len - o < sizeof(chunk) ? len - o : sizeof(chunk);
        memset(chunk, 0xFF, sizeof(chunk));
        memcpy(chunk, data + o, n);
        if (bflb_flash_write(lb_addr() + 8 + (uint32_t)o, chunk, (n + 3) & ~3u) != 0) return false;
    }
    return true;
}

/* ---------------- U2F attestation key ---------------- */
#define ATT_MAGIC 0x474B4154u /* "GKAT" */

bool store_att_load(uint8_t blob[64], uint8_t pub[64], uint8_t *cert, size_t *certLen)
{
    static uint8_t b[4 + 64 + 64 + 2 + STORE_ATT_CERT_MAX];
    if (!store_ready || bflb_flash_read(att_addr(), b, sizeof(b)) != 0 || get_u32(b) != ATT_MAGIC) return false;
    size_t n = get_u16(b + 132);
    if (n == 0 || n > STORE_ATT_CERT_MAX) return false;
    memcpy(blob, b + 4, 64);
    memcpy(pub, b + 68, 64);
    memcpy(cert, b + 134, n);
    *certLen = n;
    return true;
}

bool store_att_save(const uint8_t blob[64], const uint8_t pub[64], const uint8_t *cert, size_t certLen)
{
    if (!store_ready || certLen == 0 || certLen > STORE_ATT_CERT_MAX) return false;
    static uint8_t b[4 + 64 + 64 + 2 + STORE_ATT_CERT_MAX];
    memset(b, 0xFF, sizeof(b));
    put_u32(b, ATT_MAGIC);
    memcpy(b + 4, blob, 64);
    memcpy(b + 68, pub, 64);
    put_u16(b + 132, (uint16_t)certLen);
    memcpy(b + 134, cert, certLen);
    if (bflb_flash_erase(att_addr(), SECTOR_SIZE) != 0) return false;
    return bflb_flash_write(att_addr(), b, (134 + certLen + 3) & ~3u) == 0;
}
