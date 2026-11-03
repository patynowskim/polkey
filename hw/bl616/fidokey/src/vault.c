#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "cJSON.h"
#include "vault.h"
#include "ctaphid.h"
#include "ctap2.h"
#include "ctap2_int.h"
#include "pin.h"
#include "sha256.h"
#include "sha1.h"
#include "gcm.h"
#include "fpga.h"
#include "fp.h"
#include "oled.h"
#include "store.h"
#include "dirtyjtag.h"

#define PBKDF2_ITERATIONS 20000
#define MIN_PIN_LEN 4
#define MAX_PIN_LEN 63
#define MAX_FAILED_PINS 10
#define SESSION_TIMEOUT_MS (5 * 60 * 1000)

#define ENTRY_SIZE 256u
#define ENTRIES_PER_SECTOR (4096u / ENTRY_SIZE)
#define ENTRY_SECTORS (STORE_VAULT_SECTORS - 1)
#define MAX_ENTRIES (ENTRY_SECTORS * ENTRIES_PER_SECTOR)
#define E_USED 0xA5
#define T_PASS 1
#define T_TOTP 2
#define SITE_MAX 64
#define LOGIN_MAX 48
#define SECRET_MAX 96

/* entry: 0 magic | 1 type | 2 ctLen | 4 site[64] | 68 login[48] | 116 iv[12] | 128 tag[16] | 144 ct[96] */
#define E_TYPE 1
#define E_CTLEN 2
#define E_SITE 4
#define E_LOGIN 68
#define E_IV 116
#define E_TAG 128
#define E_CT 144

#define META_MAGIC 0x474B5654u /* "GKVT" */

typedef struct {
    bool pinSet;
    uint8_t failures;
    int16_t alg;
    bool upOff;            /* user presence (button) switched off by the owner */
    uint8_t salt[16];
    uint8_t verifier[32];
    uint8_t devBlob[64];
    uint8_t devPub[64];
} vmeta_t;

static vmeta_t vm;
static bool authed;
static uint8_t vkey[32];
static TickType_t last_activity;
static uint8_t sec_buf[4096];

/* ---------------- tiny helpers ---------------- */
static __attribute__((noinline)) uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static __attribute__((noinline)) void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static __attribute__((noinline)) bool meta_load(void)
{
    uint8_t b[256];
    memset(&vm, 0, sizeof(vm));
    if (!store_vault_read(0, b, sizeof(b)) || rd32(b) != META_MAGIC) return false;
    vm.pinSet = b[4] == 1;
    vm.failures = b[5];
    vm.alg = (int16_t)(b[6] | (b[7] << 8));
    memcpy(vm.salt, b + 8, 16);
    memcpy(vm.verifier, b + 24, 32);
    memcpy(vm.devBlob, b + 56, 64);
    memcpy(vm.devPub, b + 120, 64);
    vm.upOff = b[184] == 1;
    return true;
}

static __attribute__((noinline)) bool meta_save(void)
{
    STAGE(120);
    if (!store_vault_read(0, sec_buf, 4096)) return false; /* sector 0 holds only the metadata */
    STAGE(121);
    memset(sec_buf, 0xFF, 4096);
    wr32(sec_buf, META_MAGIC);
    sec_buf[4] = vm.pinSet ? 1 : 0;
    sec_buf[5] = vm.failures;
    sec_buf[6] = (uint8_t)vm.alg;
    sec_buf[7] = (uint8_t)((uint16_t)vm.alg >> 8);
    memcpy(sec_buf + 8, vm.salt, 16);
    memcpy(sec_buf + 24, vm.verifier, 32);
    memcpy(sec_buf + 56, vm.devBlob, 64);
    memcpy(sec_buf + 120, vm.devPub, 64);
    sec_buf[184] = vm.upOff ? 1 : 0;
    STAGE(122);
    bool wr = store_vault_write_sector(0, sec_buf);
    STAGE(123);
    return wr;
}

void vault_init(void)
{
    meta_load();
    authed = false;
    memset(vkey, 0, sizeof(vkey));
}

int vault_default_alg(void)
{
    return vm.alg;
}

bool vault_unlocked(void)
{
    return authed;
}

bool vault_up_required(void)
{
    return !vm.upOff;
}

static __attribute__((noinline)) void lock(void)
{
    authed = false;
    memset(vkey, 0, sizeof(vkey));
}

/* ---------------- JSON out ---------------- */
static vault_reply_fn reply_fn;
static void *reply_ctx;

static __attribute__((noinline)) void send_json(uint32_t ch, cJSON *root)
{
    (void)ch;
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    static const char oom[] = "{\"type\":\"error\",\"module\":\"SYS\",\"error_code\":\"OUT_OF_MEMORY\"}";
    if (!s) {
        reply_fn(reply_ctx, (const uint8_t *)oom, sizeof(oom) - 1);
        return;
    }
    size_t n = strlen(s);
    if (n > CTAPHID_MAX_MSG) n = 0;
    reply_fn(reply_ctx, (const uint8_t *)s, n);
    free(s);
}

static __attribute__((noinline)) void send_event(uint32_t ch, const char *module, const char *event, cJSON *data)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "event");
    cJSON_AddStringToObject(r, "module", module);
    cJSON_AddStringToObject(r, "event", event);
    if (data) cJSON_AddItemToObject(r, "data", data);
    send_json(ch, r);
}

static __attribute__((noinline)) void send_error(uint32_t ch, const char *module, const char *code, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "error");
    cJSON_AddStringToObject(r, "module", module);
    cJSON_AddStringToObject(r, "error_code", code);
    if (msg) cJSON_AddStringToObject(r, "message", msg);
    send_json(ch, r);
}

/* first present string among up to three keys */
static __attribute__((noinline)) const char *jstr(const cJSON *req, const char *k1, const char *k2, const char *k3)
{
    const char *keys[3] = { k1, k2, k3 };
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) continue;
        cJSON *it = cJSON_GetObjectItemCaseSensitive(req, keys[i]);
        if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    }
    return "";
}

static __attribute__((noinline)) bool jbool(const cJSON *req, const char *k)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(req, k);
    return cJSON_IsTrue(it);
}

static __attribute__((noinline)) double jnum(const cJSON *req, const char *k, double def)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(req, k);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

/* ---------------- key derivation ---------------- */
static __attribute__((noinline)) bool device_secret(uint8_t out[32])
{
    return fpga_p256_ecdh(vm.devBlob, vm.devPub, out);
}

/* PIN -> verifier tag and vault key */
static __attribute__((noinline)) bool derive(const char *pin, const uint8_t salt[16], uint8_t verifier[32], uint8_t key[32])
{
    uint8_t k[64], dev[32], ikm[64];
    if (!device_secret(dev)) return false;
    STAGE(113);
    pbkdf2_sha256((const uint8_t *)pin, strlen(pin), salt, 16, PBKDF2_ITERATIONS, k, 64);
    sha256_ctx_t c;
    STAGE(114);
    sha256_begin(&c);
    sha256_feed(&c, (const uint8_t *)"vault-verify", 12);
    sha256_feed(&c, k + 32, 32);
    sha256_end(&c, verifier);
    memcpy(ikm, k, 32);
    memcpy(ikm + 32, dev, 32);
    STAGE(115);
    hkdf_sha256_32(salt, 16, ikm, 64, "PolKey vault key", key);
    STAGE(119);
    memset(k, 0, sizeof(k));
    memset(dev, 0, sizeof(dev));
    memset(ikm, 0, sizeof(ikm));
    return true;
}

/* ---------------- entries ---------------- */
static __attribute__((noinline)) bool entry_read(unsigned idx, uint8_t e[ENTRY_SIZE])
{
    return store_vault_read(4096u + idx * ENTRY_SIZE, e, ENTRY_SIZE);
}

static __attribute__((noinline)) bool entry_write(unsigned idx, const uint8_t e[ENTRY_SIZE])
{
    unsigned sector = 1 + idx / ENTRIES_PER_SECTOR;
    if (!store_vault_read(sector * 4096u, sec_buf, 4096)) return false;
    memcpy(sec_buf + (idx % ENTRIES_PER_SECTOR) * ENTRY_SIZE, e, ENTRY_SIZE);
    return store_vault_write_sector(sector, sec_buf);
}

static __attribute__((noinline)) bool entry_clear(unsigned idx)
{
    uint8_t e[ENTRY_SIZE];
    memset(e, 0xFF, sizeof(e));
    return entry_write(idx, e);
}

static __attribute__((noinline)) int entry_find(uint8_t type, const char *site, const char *login)
{
    uint8_t e[ENTRY_SIZE];
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (!entry_read(i, e) || e[0] != E_USED || e[E_TYPE] != type) continue;
        if (strncmp((const char *)e + E_SITE, site, SITE_MAX) == 0 &&
            (type == T_TOTP || strncmp((const char *)e + E_LOGIN, login, LOGIN_MAX) == 0)) {
            return (int)i;
        }
    }
    return -1;
}

static __attribute__((noinline)) int entry_free(void)
{
    uint8_t e[ENTRY_SIZE];
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (entry_read(i, e) && e[0] != E_USED) return (int)i;
    }
    return -1;
}

static __attribute__((noinline)) void entry_aad(const uint8_t *e, uint8_t type, uint8_t aad[1 + SITE_MAX + LOGIN_MAX], size_t *n)
{
    aad[0] = type;
    memcpy(aad + 1, e + E_SITE, SITE_MAX);
    memcpy(aad + 1 + SITE_MAX, e + E_LOGIN, LOGIN_MAX);
    *n = 1 + SITE_MAX + LOGIN_MAX;
}

/* store (or replace) an entry; secret is a NUL-terminated string */
static __attribute__((noinline)) bool entry_put(int idx, uint8_t type, const char *site, const char *login, const char *secret)
{
    size_t sl = strlen(secret);
    if (sl == 0 || sl > SECRET_MAX || strlen(site) >= SITE_MAX || strlen(login) >= LOGIN_MAX) return false;
    uint8_t e[ENTRY_SIZE], aad[1 + SITE_MAX + LOGIN_MAX];
    size_t an;
    memset(e, 0, sizeof(e));
    e[0] = E_USED;
    e[E_TYPE] = type;
    e[E_CTLEN] = (uint8_t)sl;
    strncpy((char *)e + E_SITE, site, SITE_MAX - 1);
    strncpy((char *)e + E_LOGIN, login, LOGIN_MAX - 1);
    ctap2_random(e + E_IV, 12);
    entry_aad(e, type, aad, &an);
    aes256_gcm_encrypt(vkey, e + E_IV, aad, an, (const uint8_t *)secret, sl, e + E_CT, e + E_TAG);
    return entry_write((unsigned)idx, e);
}

/* decrypt into out (NUL-terminated, cap >= SECRET_MAX + 1) */
static __attribute__((noinline)) bool entry_secret(const uint8_t *e, char *out)
{
    uint8_t aad[1 + SITE_MAX + LOGIN_MAX];
    size_t an, n = e[E_CTLEN];
    if (n == 0 || n > SECRET_MAX) return false;
    entry_aad(e, e[E_TYPE], aad, &an);
    if (!aes256_gcm_decrypt(vkey, e + E_IV, aad, an, e + E_CT, n, e + E_TAG, (uint8_t *)out)) return false;
    out[n] = '\0';
    return true;
}

static __attribute__((noinline)) int count_entries(uint8_t type, size_t *loginChars)
{
    uint8_t e[ENTRY_SIZE];
    int n = 0;
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (entry_read(i, e) && e[0] == E_USED && e[E_TYPE] == type) {
            n++;
            if (loginChars) *loginChars += strnlen((const char *)e + E_LOGIN, LOGIN_MAX);
        }
    }
    return n;
}

/* ---------------- password generator / TOTP ---------------- */
static __attribute__((noinline)) void gen_password(char *out, size_t length)
{
    static const char lower[] = "abcdefghijkmnopqrstuvwxyz", upper[] = "ABCDEFGHIJKLMNPQRSTUVWXYZ";
    static const char num[] = "23456789", sym[] = "!@#$%^*()-_=+";
    static const char all[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHIJKLMNPQRSTUVWXYZ23456789!@#$%^*()-_=+";
    uint8_t r[32];
    ctap2_random(r, sizeof(r));
    out[0] = lower[r[0] % (sizeof(lower) - 1)];
    out[1] = upper[r[1] % (sizeof(upper) - 1)];
    out[2] = num[r[2] % (sizeof(num) - 1)];
    out[3] = sym[r[3] % (sizeof(sym) - 1)];
    for (size_t i = 4; i < length; i++) out[i] = all[r[i] % (sizeof(all) - 1)];
    for (size_t i = length - 1; i > 0; i--) { /* shuffle */
        uint8_t b;
        ctap2_random(&b, 1);
        size_t j = b % (i + 1);
        char t = out[i];
        out[i] = out[j];
        out[j] = t;
    }
    out[length] = '\0';
    memset(r, 0, sizeof(r));
}

static __attribute__((noinline)) int base32_decode(const char *s, uint8_t *out, size_t cap)
{
    uint32_t buf = 0;
    int bits = 0;
    size_t n = 0;
    for (; *s; s++) {
        int v;
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else continue; /* ignore padding, spaces, dashes */
        buf = (buf << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(buf >> bits);
        }
    }
    return (int)n;
}

static __attribute__((noinline)) bool totp(const char *secret, uint32_t epoch, char out[8])
{
    uint8_t key[64], mac[20], t[8];
    int kl = base32_decode(secret, key, sizeof(key));
    if (kl <= 0) return false;
    uint64_t step = epoch / 30;
    for (int i = 7; i >= 0; i--) { t[i] = (uint8_t)step; step >>= 8; }
    hmac_sha1(key, (size_t)kl, t, 8, mac);
    int o = mac[19] & 0x0F;
    uint32_t bin = ((uint32_t)(mac[o] & 0x7F) << 24) | ((uint32_t)mac[o + 1] << 16) | ((uint32_t)mac[o + 2] << 8) | mac[o + 3];
    snprintf(out, 8, "%06lu", (unsigned long)(bin % 1000000));
    memset(key, 0, sizeof(key));
    return true;
}

static __attribute__((noinline)) bool is_weak(const char *pw, const char all[][SECRET_MAX + 1], int n, int self)
{
    size_t l = strlen(pw);
    bool weak = l < 12;
    if (!weak) {
        bool num = false, up = false, lo = false, sp = false;
        for (size_t i = 0; i < l; i++) {
            char c = pw[i];
            if (c >= '0' && c <= '9') num = true;
            else if (c >= 'A' && c <= 'Z') up = true;
            else if (c >= 'a' && c <= 'z') lo = true;
            else sp = true;
        }
        weak = !(num && up && lo && sp);
    }
    int freq = 0;
    for (int j = 0; j < n; j++) {
        if (strcmp(all[j], pw) == 0) freq++;
    }
    (void)self;
    return weak || freq > 1;
}

/* ---------------- FIDO (resident credential) management ---------------- */
static __attribute__((noinline)) void list_fido(uint32_t ch)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *sites = cJSON_AddArrayToObject(data, "websites");
    cred_record_t rec;
    static char seen[STORE_MAX_CRED][STORE_RP_ID_MAX]; /* not on the task stack: 8 KB */
    int nseen = 0, used = 0;
    for (int i = store_iter(0, &rec); i >= 0; i = store_iter(i + 1, &rec)) {
        if (rec.flags & STORE_FLAG_U2F) continue;
        used++;
        bool dup = false;
        for (int j = 0; j < nseen; j++) {
            if (strcmp(seen[j], rec.rpId) == 0) { dup = true; break; }
        }
        if (!dup && nseen < STORE_MAX_CRED) {
            strncpy(seen[nseen], rec.rpId, STORE_RP_ID_MAX);
            cJSON_AddItemToArray(sites, cJSON_CreateString(rec.rpId));
            nseen++;
        }
    }
    cJSON_AddNumberToObject(data, "discoverable_credentials_used", used);
    cJSON_AddNumberToObject(data, "discoverable_credentials_remaining", store_capacity() - used);
    send_event(ch, "FIDO2", "LIST", data);
}

static __attribute__((noinline)) bool delete_fido(const char *site)
{
    cred_record_t rec;
    uint8_t ids[STORE_MAX_CRED][STORE_CRED_ID_LEN];
    int n = 0;
    for (int i = store_iter(0, &rec); i >= 0 && n < STORE_MAX_CRED; i = store_iter(i + 1, &rec)) {
        if (strcmp(rec.rpId, site) == 0) memcpy(ids[n++], rec.credId, STORE_CRED_ID_LEN);
    }
    for (int i = 0; i < n; i++) store_delete_by_credid(ids[i]);
    return n > 0;
}

/* ---------------- diagnostics ---------------- */
static __attribute__((noinline)) int run_diagnostics(void)
{
    int passed = 0;
    if (fpga_available()) passed++;
    cred_record_t rec;
    store_iter(0, &rec);
    if (store_capacity() > 0) passed++; /* store reachable */
    uint8_t a[16], b[16];
    ctap2_random(a, 16);
    ctap2_random(b, 16);
    if (memcmp(a, b, 16) != 0) passed++;
    uint8_t h[32];
    ctap2_sha256((const uint8_t *)"abc", 3, h);
    if (h[0] == 0xba && h[1] == 0x78 && h[31] == 0xad) passed++;
    return passed;
}

/* ---------------- command dispatch ---------------- */
static __attribute__((noinline)) void factory_reset(void)
{
    lock();
    store_vault_erase();
    store_wipe();
    pin_factory_reset();
    ctap2_mgmt_reset();
    memset(&vm, 0, sizeof(vm));
}


/* ---------------- backup (format VBK1, compatible with the ESP32 firmware) ---------------- */
#define BK_ITERATIONS 200000u
#define BK_HEADER_LEN (4 + 1 + 16 + 4 + 12 + 16 + 4)
#define BK_CHUNK 256
#define BK_MAX_HEX (192u * 1024u)

static __attribute__((noinline)) bool bk_key(const char *pass, const uint8_t salt[16], uint32_t iterations, uint8_t key[32])
{
    size_t pl = strlen(pass);
    if (pl <= 64 && fpga_pbkdf2_sha256((const uint8_t *)pass, pl, salt, 16, iterations, key, 32)) return true; /* ~1 s on the FPGA */
    pbkdf2_sha256((const uint8_t *)pass, pl, salt, 16, iterations, key, 32);
    return true;
}

static __attribute__((noinline)) void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static __attribute__((noinline)) int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static __attribute__((noinline)) void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static __attribute__((noinline)) uint32_t get_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static __attribute__((noinline)) cJSON *vault_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *pw = cJSON_AddArrayToObject(root, "passwords");
    cJSON *tt = cJSON_AddArrayToObject(root, "totp");
    uint8_t e[ENTRY_SIZE];
    char sec[SECRET_MAX + 1];
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (!entry_read(i, e) || e[0] != E_USED || !entry_secret(e, sec)) continue;
        cJSON *it = cJSON_CreateObject();
        if (e[E_TYPE] == T_PASS) {
            cJSON_AddStringToObject(it, "site", (const char *)e + E_SITE);
            cJSON_AddStringToObject(it, "login", (const char *)e + E_LOGIN);
            cJSON_AddStringToObject(it, "password", sec);
            cJSON_AddItemToArray(pw, it);
        } else {
            cJSON_AddStringToObject(it, "name", (const char *)e + E_SITE);
            cJSON_AddStringToObject(it, "secret", sec);
            cJSON_AddItemToArray(tt, it);
        }
    }
    memset(sec, 0, sizeof(sec));
    cJSON_AddNumberToObject(root, "crypto_alg", vm.alg ? vm.alg : -7);
    cJSON_AddNumberToObject(root, "backup_format", 1);
    return root;
}

static __attribute__((noinline)) void backup_export(uint32_t ch, const char *pass)
{
    if (strlen(pass) < 8) { send_error(ch, "BACKUP", "EXPORT_FAILED", "PASSPHRASE_TOO_SHORT"); return; }

    cJSON *root = vault_to_json();
    char *pt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!pt) { send_error(ch, "BACKUP", "EXPORT_FAILED", "OOM"); return; }
    size_t ptLen = strlen(pt);

    uint8_t salt[16], nonce[12], key[32], tag[16];
    ctap2_random(salt, 16);
    ctap2_random(nonce, 12);
    size_t total = BK_HEADER_LEN + ptLen;
    uint8_t *blob = (uint8_t *)malloc(total);
    char *hex = (char *)malloc(total * 2 + 1);
    if (!blob || !hex) {
        free(blob); free(hex); free(pt);
        send_error(ch, "BACKUP", "EXPORT_FAILED", "OOM");
        return;
    }
    bk_key(pass, salt, BK_ITERATIONS, key);
    aes256_gcm_encrypt(key, nonce, NULL, 0, (const uint8_t *)pt, ptLen, blob + BK_HEADER_LEN, tag);
    memset(key, 0, sizeof(key));
    memset(pt, 0, ptLen);
    free(pt);

    size_t o = 0;
    memcpy(blob, "VBK1", 4); o = 4;
    blob[o++] = 1;
    memcpy(blob + o, salt, 16); o += 16;
    put_be32(blob + o, BK_ITERATIONS); o += 4;
    memcpy(blob + o, nonce, 12); o += 12;
    memcpy(blob + o, tag, 16); o += 16;
    put_be32(blob + o, (uint32_t)ptLen);
    hex_encode(blob, total, hex);
    memset(blob, 0, total);
    free(blob);

    size_t hl = total * 2;
    uint8_t digest[32];
    ctap2_sha256((const uint8_t *)hex, hl, digest); /* hashed as a string, like the ESP32 firmware */
    char dhex[65];
    hex_encode(digest, 32, dhex);

    size_t chunks = (hl + BK_CHUNK - 1) / BK_CHUNK;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "total_len", (double)hl);
    cJSON_AddNumberToObject(d, "total_chunks", (double)chunks);
    cJSON_AddStringToObject(d, "sha256", dhex);
    send_event(ch, "BACKUP", "EXPORT_START", d);
    for (size_t i = 0; i < chunks; i++) {
        size_t start = i * BK_CHUNK, n = hl - start < BK_CHUNK ? hl - start : BK_CHUNK;
        char piece[BK_CHUNK + 1];
        memcpy(piece, hex + start, n);
        piece[n] = '\0';
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "seq", (double)i);
        cJSON_AddStringToObject(c, "data", piece);
        send_event(ch, "BACKUP", "EXPORT_CHUNK", c);
    }
    cJSON *done = cJSON_CreateObject();
    cJSON_AddNumberToObject(done, "total_chunks", (double)chunks);
    send_event(ch, "BACKUP", "EXPORT_DONE", done);
    memset(hex, 0, hl);
    free(hex);
}

static char *imp_hex;
static size_t imp_total, imp_got;
static char imp_sha[65];

static __attribute__((noinline)) void backup_abort(void)
{
    if (imp_hex) {
        memset(imp_hex, 0, imp_total);
        free(imp_hex);
    }
    imp_hex = NULL;
    imp_total = imp_got = 0;
    imp_sha[0] = '\0';
}

static __attribute__((noinline)) void backup_import_commit(uint32_t ch, const char *pass, bool overwrite)
{
    const char *err = NULL;
    uint8_t *blob = NULL;
    char *plain = NULL;
    cJSON *doc = NULL;
    uint32_t imported = 0, skipped = 0;

    if (!imp_hex) { err = "NOT_STARTED"; goto fail; }
    if (imp_got != imp_total) { err = "INCOMPLETE"; goto fail; }

    uint8_t digest[32];
    char dhex[65];
    ctap2_sha256((const uint8_t *)imp_hex, imp_total, digest);
    hex_encode(digest, 32, dhex);
    if (strcmp(dhex, imp_sha) != 0) { err = "CHECKSUM_MISMATCH"; goto fail; }

    size_t n = imp_total / 2;
    if (imp_total % 2 || n < BK_HEADER_LEN) { err = "MALFORMED_BLOB"; goto fail; }
    blob = (uint8_t *)malloc(n);
    if (!blob) { err = "OOM"; goto fail; }
    for (size_t i = 0; i < n; i++) {
        int a = hex_val(imp_hex[2 * i]), b = hex_val(imp_hex[2 * i + 1]);
        if (a < 0 || b < 0) { err = "BAD_HEX"; goto fail; }
        blob[i] = (uint8_t)(a << 4 | b);
    }
    backup_abort();

    if (memcmp(blob, "VBK1", 4) != 0) { err = "BAD_MAGIC"; goto fail; }
    if (blob[4] != 1) { err = "UNSUPPORTED_VERSION"; goto fail; }
    uint32_t iters = get_be32(blob + 21);
    uint32_t ctLen = get_be32(blob + 53);
    if (iters == 0 || iters > 2000000u || ctLen != n - BK_HEADER_LEN) { err = "MALFORMED_BLOB"; goto fail; }

    plain = (char *)malloc(ctLen + 1);
    if (!plain) { err = "OOM"; goto fail; }
    uint8_t key[32];
    bk_key(pass, blob + 5, iters, key);
    bool ok = aes256_gcm_decrypt(key, blob + 25, NULL, 0, blob + BK_HEADER_LEN, ctLen, blob + 37, (uint8_t *)plain);
    memset(key, 0, sizeof(key));
    if (!ok) { err = "DECRYPT_FAILED"; goto fail; }
    plain[ctLen] = '\0';

    doc = cJSON_Parse(plain);
    memset(plain, 0, ctLen);
    if (!doc) { err = "BAD_ARCHIVE_JSON"; goto fail; }

    cJSON *it;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(doc, "passwords");
    cJSON_ArrayForEach(it, arr) {
        const char *site = jstr(it, "site", NULL, NULL), *login = jstr(it, "login", NULL, NULL), *pw = jstr(it, "password", NULL, NULL);
        if (!*site || !*login) { skipped++; continue; }
        int idx = entry_find(T_PASS, site, login);
        if (idx >= 0 && !overwrite) { skipped++; continue; }
        if (idx < 0) idx = entry_free();
        if (idx >= 0 && entry_put(idx, T_PASS, site, login, pw)) imported++; else skipped++;
    }
    arr = cJSON_GetObjectItemCaseSensitive(doc, "totp");
    cJSON_ArrayForEach(it, arr) {
        const char *name = jstr(it, "name", NULL, NULL), *secret = jstr(it, "secret", NULL, NULL);
        if (!*name || !*secret) { skipped++; continue; }
        int idx = entry_find(T_TOTP, name, "");
        if (idx >= 0 && !overwrite) { skipped++; continue; }
        if (idx < 0) idx = entry_free();
        if (idx >= 0 && entry_put(idx, T_TOTP, name, "", secret)) imported++; else skipped++;
    }
    cJSON_Delete(doc);
    free(plain);
    free(blob);

    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "imported", imported);
    cJSON_AddNumberToObject(d, "skipped", skipped);
    send_event(ch, "BACKUP", "IMPORT_DONE", d);
    return;

fail:
    if (doc) cJSON_Delete(doc);
    if (plain) { free(plain); }
    if (blob) { memset(blob, 0, 64); free(blob); }
    backup_abort();
    send_error(ch, "BACKUP", "IMPORT_FAILED", err);
}

static __attribute__((noinline)) void backup_command(uint32_t ch, const char *cmd, const cJSON *req)
{
    if (strcmp(cmd, "BACKUP_EXPORT") == 0) {
        backup_export(ch, jstr(req, "passphrase", NULL, NULL));
    } else if (strcmp(cmd, "BACKUP_IMPORT_BEGIN") == 0) {
        backup_abort();
        uint32_t total = (uint32_t)jnum(req, "total_len", 0);
        const char *sha = jstr(req, "sha256", NULL, NULL);
        if (total == 0 || total > BK_MAX_HEX) { send_error(ch, "BACKUP", "IMPORT_BEGIN_FAILED", "BAD_LENGTH"); return; }
        if (strlen(sha) != 64) { send_error(ch, "BACKUP", "IMPORT_BEGIN_FAILED", "BAD_CHECKSUM_FORMAT"); return; }
        imp_hex = (char *)malloc(total + 1);
        if (!imp_hex) { send_error(ch, "BACKUP", "IMPORT_BEGIN_FAILED", "OOM"); return; }
        imp_total = total;
        imp_got = 0;
        strcpy(imp_sha, sha);
        send_event(ch, "BACKUP", "IMPORT_READY", NULL);
    } else if (strcmp(cmd, "BACKUP_IMPORT_CHUNK") == 0) {
        const char *data = jstr(req, "data", NULL, NULL);
        size_t n = strlen(data);
        if (!imp_hex) { send_error(ch, "BACKUP", "IMPORT_CHUNK_FAILED", "NOT_STARTED"); return; }
        if (imp_got + n > imp_total) {
            backup_abort();
            send_error(ch, "BACKUP", "IMPORT_CHUNK_FAILED", "OVERFLOW");
            return;
        }
        memcpy(imp_hex + imp_got, data, n);
        imp_got += n;
    } else if (strcmp(cmd, "BACKUP_IMPORT_COMMIT") == 0) {
        backup_import_commit(ch, jstr(req, "passphrase", NULL, NULL), jbool(req, "overwrite"));
    } else if (strcmp(cmd, "BACKUP_IMPORT_ABORT") == 0) {
        backup_abort();
        send_event(ch, "BACKUP", "IMPORT_ABORTED", NULL);
    }
}

static __attribute__((noinline)) bool verify_pin_value(uint32_t ch, const char *pin, uint8_t keyOut[32])
{
    uint8_t ver[32];
    if (!derive(pin, vm.salt, ver, keyOut)) {
        send_error(ch, "SECURITY", "FPGA_REQUIRED", "The FPGA accelerator is needed to unlock the vault.");
        return false;
    }
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= (uint8_t)(ver[i] ^ vm.verifier[i]);
    if (d == 0) {
        if (vm.failures) { vm.failures = 0; meta_save(); }
        return true;
    }
    vm.failures++;
    meta_save();
    memset(keyOut, 0, 32);
    if (vm.failures >= MAX_FAILED_PINS) {
        send_error(ch, "SECURITY", "MAX_ATTEMPTS_EXCEEDED", "Maximum PIN attempts reached. Device wiped.");
        factory_reset();
    } else {
        char m[40];
        snprintf(m, sizeof(m), "%d/%d attempts used.", vm.failures, MAX_FAILED_PINS);
        send_error(ch, "SECURITY", "BAD_PIN_ATTEMPT", m);
    }
    return false;
}

static __attribute__((noinline)) void cmd_list_pass(uint32_t ch)
{
    static char pw[MAX_ENTRIES][SECRET_MAX + 1];
    static int idxOf[MAX_ENTRIES];
    int n = 0;
    uint8_t e[ENTRY_SIZE];
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (!entry_read(i, e) || e[0] != E_USED || e[E_TYPE] != T_PASS) continue;
        if (!entry_secret(e, pw[n])) pw[n][0] = 0;
        idxOf[n++] = (int)i;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(data, "items");
    for (int k = 0; k < n; k++) {
        entry_read((unsigned)idxOf[k], e);
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "website", (const char *)e + E_SITE);
        cJSON_AddStringToObject(it, "login", (const char *)e + E_LOGIN);
        cJSON_AddStringToObject(it, "status", is_weak(pw[k], (const char (*)[SECRET_MAX + 1])pw, n, k) ? "WEAK" : "OK");
        cJSON_AddItemToArray(items, it);
    }
    memset(pw, 0, sizeof(pw));
    send_event(ch, "PASS", "LIST", data);
}

/* ESP32: fingerprint; here the button of the board. Announces the wait so that the UI can show it. */
static __attribute__((noinline)) bool touch_approval(uint32_t ch)
{
    if (!ctap2_up_required()) return true;
    send_event(ch, "PASS", "AWAITING_HARDWARE_APPROVAL", NULL);
    return ctap2_wait_up(0, 30000) == UP_OK;
}

static __attribute__((noinline)) void cmd_save_or_update(uint32_t ch, const cJSON *req, bool update)
{
    const char *site = jstr(req, "site", "website", NULL), *login = jstr(req, "login", NULL, NULL);
    bool autogen = jbool(req, "autogen");
    char gen[24];
    const char *pass = jstr(req, "pass", "password", NULL);
    if (autogen) {
        gen_password(gen, 16);
        pass = gen;
    }
    if (!*site || !*login || !*pass) {
        send_error(ch, "PASS", "MISSING_ARGS", NULL);
        return;
    }
    int idx = entry_find(T_PASS, site, login);
    if (update && idx < 0) {
        send_error(ch, "PASS", "NOT_FOUND", NULL);
        return;
    }
    if (update && !touch_approval(ch)) {
        send_error(ch, "AUTH", "UP_TIMEOUT", "No touch within 30 s.");
        return;
    }
    if (idx < 0) idx = entry_free();
    if (idx < 0) {
        send_error(ch, "PASS", "STORAGE_FULL", NULL);
        return;
    }
    if (!entry_put(idx, T_PASS, site, login, pass)) {
        send_error(ch, "PASS", "SAVE_FAILED", NULL);
        return;
    }
    cJSON *data = NULL;
    if (autogen) {
        data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, update ? "password" : "generated_password", gen);
    }
    send_event(ch, "PASS", update ? "UPDATED" : "SAVED", data);
    memset(gen, 0, sizeof(gen));
}

static __attribute__((noinline)) void cmd_get_totp(uint32_t ch, uint32_t epoch)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *codes = cJSON_AddObjectToObject(data, "codes");
    uint8_t e[ENTRY_SIZE];
    char sec[SECRET_MAX + 1], code[8];
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (!entry_read(i, e) || e[0] != E_USED || e[E_TYPE] != T_TOTP) continue;
        if (entry_secret(e, sec) && totp(sec, epoch, code)) {
            cJSON_AddStringToObject(codes, (const char *)e + E_SITE, code);
        }
    }
    memset(sec, 0, sizeof(sec));
    send_event(ch, "TOTP", "CODES", data);
}

/* re-encrypt every entry under a new key (PIN change) */
static __attribute__((noinline)) bool reencrypt_all(const uint8_t newKey[32])
{
    uint8_t e[ENTRY_SIZE], aad[1 + SITE_MAX + LOGIN_MAX];
    char sec[SECRET_MAX + 1];
    size_t an;
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (!entry_read(i, e) || e[0] != E_USED) continue;
        if (!entry_secret(e, sec)) return false;
        ctap2_random(e + E_IV, 12);
        entry_aad(e, e[E_TYPE], aad, &an);
        aes256_gcm_encrypt(newKey, e + E_IV, aad, an, (const uint8_t *)sec, strlen(sec), e + E_CT, e + E_TAG);
        if (!entry_write(i, e)) return false;
    }
    memset(sec, 0, sizeof(sec));
    return true;
}

static uint8_t last_msg[CTAPHID_MAX_MSG];
static size_t last_len;

static __attribute__((noinline)) void ctaphid_reply(void *ctx, const uint8_t *msg, size_t len)
{
    /* CTAPHID carries one message per request: keep the last one (streamed answers need the bulk tunnel) */
    (void)ctx;
    if (len <= sizeof(last_msg)) {
        memcpy(last_msg, msg, len);
        last_len = len;
    }
    return;
}

void vault_handle(uint32_t channel, const uint8_t *data, size_t len)
{
    last_len = 0;
    vault_process(data, len, ctaphid_reply, NULL);
    ctaphid_send(channel, CTAPHID_VAULT, last_msg, (uint16_t)last_len);
}

void vault_process(const uint8_t *data, size_t len, vault_reply_fn reply, void *ctx)
{
    STAGE(100);
    uint32_t ch = 0;
    reply_fn = reply;
    reply_ctx = ctx;
    char *text = (char *)malloc(len + 1);
    if (!text) {
        send_error(ch, "SYS", "OUT_OF_MEMORY", NULL);
        return;
    }
    memcpy(text, data, len);
    text[len] = '\0';
    cJSON *req = cJSON_Parse(text);
    memset(text, 0, len);
    free(text);
    if (!req) {
        send_error(ch, "SYS", "INVALID_JSON", NULL);
        return;
    }
    const char *cmd = jstr(req, "cmd", NULL, NULL);

    if (authed && (xTaskGetTickCount() - last_activity) > pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) lock();
    last_activity = xTaskGetTickCount();

    if (strcmp(cmd, "BENCH") == 0) { /* timing of the building blocks of CREATE_PIN (diagnostics, works before a PIN exists) */
        TickType_t t0 = xTaskGetTickCount();
        uint8_t out[32], salt[16] = { 1 }, d[32];
        pbkdf2_sha256((const uint8_t *)"bench", 5, salt, 16, 1000, out, 32);
        uint32_t pbkdf2_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        t0 = xTaskGetTickCount();
        uint8_t blob[64], pub[64];
        bool gk = fpga_p256_genkey(blob, pub);
        uint32_t genkey_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        t0 = xTaskGetTickCount();
        bool ec = gk && fpga_p256_ecdh(blob, pub, d);
        uint32_t ecdh_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        t0 = xTaskGetTickCount();
        ctap2_random(out, 32);
        uint32_t rnd_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "pbkdf2_1000_ms", pbkdf2_ms);
        cJSON_AddBoolToObject(r, "genkey_ok", gk);
        cJSON_AddNumberToObject(r, "genkey_ms", genkey_ms);
        cJSON_AddBoolToObject(r, "ecdh_ok", ec);
        cJSON_AddNumberToObject(r, "ecdh_ms", ecdh_ms);
        cJSON_AddNumberToObject(r, "random_ms", rnd_ms);
        cJSON_AddNumberToObject(r, "fpga_caps", fpga_caps());
        cJSON_AddBoolToObject(r, "pin_set", vm.pinSet);
        cJSON_AddNumberToObject(r, "tick_ms", portTICK_PERIOD_MS);
        uint8_t bs = 0;
        bool ux = fpga_ux_supported() && fpga_ux_buttons(false, &bs); /* bit0 pressed now, bit1 latched */
        cJSON_AddBoolToObject(r, "ux_ok", ux);
        cJSON_AddNumberToObject(r, "ux_state", bs);
        send_event(ch, "SYS", "BENCH", r);
    } else if (strcmp(cmd, "UX_STATE") == 0) { /* raw state of the touch button (diagnostics, works before a PIN exists) */
        uint8_t bs = 0;
        bool ux = fpga_ux_supported() && fpga_ux_buttons(false, &bs); /* bit0 pressed now, bit1 latched */
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ux_ok", ux);
        cJSON_AddNumberToObject(r, "ux_state", bs);
        send_event(ch, "SYS", "UX_STATE", r);
    } else if (strcmp(cmd, "RESTART_SYSTEM") == 0) {
        lock();
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "pin_set", vm.pinSet);
        send_event(ch, "SYS", "BOOT", d);
    } else if (strcmp(cmd, "DISCONNECT") == 0) {
        lock();
        send_event(ch, "SYS", "DISCONNECTED", NULL);
    } else if (!vm.pinSet) {
        if (strcmp(cmd, "CREATE_PIN") == 0 || strcmp(cmd, "set_pin") == 0) {
            const char *pin = jstr(req, "pin", NULL, NULL);
            if (strlen(pin) < MIN_PIN_LEN || strlen(pin) > MAX_PIN_LEN) {
                send_error(ch, "AUTH", "PIN_TOO_SHORT", NULL);
            } else if (!fpga_available() || !fpga_has_asym()) {
                send_error(ch, "AUTH", "FPGA_REQUIRED", "The FPGA accelerator is needed to create the vault.");
            } else {
                uint8_t key[32];
                STAGE(110);
                if (!fpga_p256_genkey(vm.devBlob, vm.devPub)) {
                    send_error(ch, "AUTH", "KEYGEN_FAILED", NULL);
                } else {
                    STAGE(111);
                    ctap2_random(vm.salt, 16);
                    STAGE(112);
                    if (derive(pin, vm.salt, vm.verifier, key)) {
                        vm.pinSet = true;
                        vm.failures = 0;
                        STAGE(116);
                        if (meta_save()) send_event(ch, "AUTH", "PIN_CREATED", NULL);
                        else { vm.pinSet = false; send_error(ch, "AUTH", "SAVE_FAILED", NULL); }
                    } else {
                        send_error(ch, "AUTH", "KEYGEN_FAILED", NULL);
                    }
                    memset(key, 0, sizeof(key));
                }
            }
        } else {
            send_error(ch, "AUTH", "NEW_PIN_REQ", NULL);
        }
    } else if (!authed) {
        if (strcmp(cmd, "VERIFY_PIN") == 0 || strcmp(cmd, "verify_pin") == 0) {
            uint8_t key[32];
            if (verify_pin_value(ch, jstr(req, "pin", NULL, NULL), key)) {
                memcpy(vkey, key, 32);
                authed = true;
                send_event(ch, "SECURITY", "PIN_OK", NULL);
            }
            memset(key, 0, sizeof(key));
        } else {
            send_error(ch, "AUTH", "PIN_REQ", NULL);
        }
    } else if (strcmp(cmd, "help") == 0) {
        static const char *names[] = { "SAVE_PASS", "GET_PASS", "DELETE_PASS", "LIST_PASS", "SAVE_TOTP", "GET_TOTP",
            "DELETE_TOTP", "LIST_FIDO", "DELETE_FIDO", "STORAGE_INFO", "PURGE_STORAGE", "RUN_DIAGNOSTICS",
            "UPDATE_SETTINGS", "UPDATE_PASS", "SET_CRYPTO_ALG", "FACTORY_RESET", "UPDATE_PIN" };
        cJSON *d = cJSON_CreateObject();
        cJSON *a = cJSON_AddArrayToObject(d, "commands");
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) cJSON_AddItemToArray(a, cJSON_CreateString(names[i]));
        send_event(ch, "SYS", "HELP_MENU", d);
    } else if (strcmp(cmd, "SAVE_PASS") == 0 || strcmp(cmd, "create") == 0) {
        cmd_save_or_update(ch, req, false);
    } else if (strcmp(cmd, "UPDATE_PASS") == 0 || strcmp(cmd, "update") == 0) {
        cmd_save_or_update(ch, req, true);
    } else if (strcmp(cmd, "GET_PASS") == 0 || strcmp(cmd, "get") == 0) {
        int idx = entry_find(T_PASS, jstr(req, "site", "website", NULL), jstr(req, "login", NULL, NULL));
        uint8_t e[ENTRY_SIZE];
        char sec[SECRET_MAX + 1];
        if (idx >= 0 && !touch_approval(ch)) {
            send_error(ch, "AUTH", "UP_TIMEOUT", "No touch within 30 s.");
        } else if (idx >= 0 && entry_read((unsigned)idx, e) && entry_secret(e, sec)) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "password", sec);
            send_event(ch, "PASS", "TRANSMITTED", d);
            memset(sec, 0, sizeof(sec));
        } else {
            send_error(ch, "PASS", "NOT_FOUND", NULL);
        }
    } else if (strcmp(cmd, "DELETE_PASS") == 0 || strcmp(cmd, "delete") == 0) {
        int idx = entry_find(T_PASS, jstr(req, "site", "website", NULL), jstr(req, "login", NULL, NULL));
        if (idx >= 0 && entry_clear((unsigned)idx)) send_event(ch, "PASS", "DELETED", NULL);
        else send_error(ch, "PASS", "NOT_FOUND", NULL);
    } else if (strcmp(cmd, "LIST_PASS") == 0 || strcmp(cmd, "list") == 0) {
        cmd_list_pass(ch);
    } else if (strcmp(cmd, "SAVE_TOTP") == 0) {
        const char *name = jstr(req, "name", NULL, NULL), *secret = jstr(req, "secret", NULL, NULL);
        uint8_t k[64];
        if (!*name || !*secret) {
            send_error(ch, "TOTP", "MISSING_ARGS", NULL);
        } else if (base32_decode(secret, k, sizeof(k)) <= 0) {
            send_error(ch, "TOTP", "INVALID_SECRET", NULL);
        } else {
            int idx = entry_find(T_TOTP, name, "");
            if (idx < 0) idx = entry_free();
            if (idx >= 0 && entry_put(idx, T_TOTP, name, "", secret)) send_event(ch, "TOTP", "SAVED", NULL);
            else send_error(ch, "TOTP", "SAVE_FAILED", NULL);
        }
        memset(k, 0, sizeof(k));
    } else if (strcmp(cmd, "GET_TOTP") == 0) {
        cmd_get_totp(ch, (uint32_t)jnum(req, "epoch", 0));
    } else if (strcmp(cmd, "DELETE_TOTP") == 0) {
        int idx = entry_find(T_TOTP, jstr(req, "name", NULL, NULL), "");
        if (idx >= 0 && entry_clear((unsigned)idx)) send_event(ch, "TOTP", "DELETED", NULL);
        else send_error(ch, "TOTP", "NOT_FOUND", NULL);
    } else if (strcmp(cmd, "LIST_FIDO") == 0) {
        list_fido(ch);
    } else if (strcmp(cmd, "DELETE_FIDO") == 0) {
        if (delete_fido(jstr(req, "site", "rpId", NULL))) send_event(ch, "FIDO2", "DELETED", NULL);
        else send_error(ch, "FIDO2", "NOT_FOUND", NULL);
    } else if (strcmp(cmd, "STORAGE_INFO") == 0) {
        size_t chars = 0;
        int np = count_entries(T_PASS, &chars), nt = count_entries(T_TOTP, NULL);
        int pk = 0;
        cred_record_t rec;
        for (int i = store_iter(0, &rec); i >= 0; i = store_iter(i + 1, &rec)) {
            if (!(rec.flags & STORE_FLAG_U2F)) pk++;
        }
        size_t total = MAX_ENTRIES * ENTRY_SIZE, used = (size_t)(np + nt) * ENTRY_SIZE;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "total_bytes", (double)total);
        cJSON_AddNumberToObject(d, "used_bytes", (double)used);
        cJSON_AddNumberToObject(d, "free_bytes", (double)(total - used));
        cJSON_AddNumberToObject(d, "usage_percent", (double)(used * 100 / total));
        cJSON_AddNumberToObject(d, "passwords_count", np);
        cJSON_AddNumberToObject(d, "avg_login_length", np ? (double)(chars / (size_t)np) : 0);
        cJSON_AddNumberToObject(d, "passkeys_count", pk);
        send_event(ch, "STORAGE", "STATS", d);
    } else if (strcmp(cmd, "PURGE_STORAGE") == 0) {
        store_vault_erase();
        vm.pinSet = false; /* the vault key material is gone with its metadata */
        memset(vm.verifier, 0, sizeof(vm.verifier));
        lock();
        send_event(ch, "STORAGE", "PURGE_COMPLETE", NULL);
    } else if (strcmp(cmd, "RUN_DIAGNOSTICS") == 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "passed", run_diagnostics());
        cJSON_AddNumberToObject(d, "total", 4);
        send_event(ch, "SYS", "DIAGNOSTICS_COMPLETE", d);
    } else if (strcmp(cmd, "UPDATE_SETTINGS") == 0 || strcmp(cmd, "SET_CRYPTO_ALG") == 0) {
        if (cJSON_GetObjectItemCaseSensitive(req, "algId")) vm.alg = (int16_t)jnum(req, "algId", -7);
        else if (strcmp(cmd, "SET_CRYPTO_ALG") == 0) vm.alg = -7;
        if (cJSON_GetObjectItemCaseSensitive(req, "up_required")) vm.upOff = !jbool(req, "up_required");
        meta_save();
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "algId", vm.alg);
        cJSON_AddBoolToObject(d, "up_required", !vm.upOff);
        cJSON_AddBoolToObject(d, "up_button", fpga_ux_supported());
        send_event(ch, "SYS", "SETTINGS_UPDATED", d);
    } else if (strcmp(cmd, "UPDATE_PIN") == 0 || strcmp(cmd, "update_pin") == 0) {
        const char *cur = jstr(req, "current_pin", NULL, NULL), *np = jstr(req, "new_pin", NULL, NULL);
        uint8_t oldKey[32], newKey[32], ver[32], salt[16];
        if (!*cur || !*np) {
            send_error(ch, "AUTH", "MISSING_ARGS", NULL);
        } else if (verify_pin_value(ch, cur, oldKey)) {
            if (strlen(np) < MIN_PIN_LEN || strlen(np) > MAX_PIN_LEN) {
                send_error(ch, "AUTH", "PIN_TOO_SHORT", NULL);
            } else {
                ctap2_random(salt, 16);
                if (derive(np, salt, ver, newKey) && reencrypt_all(newKey)) {
                    memcpy(vm.salt, salt, 16);
                    memcpy(vm.verifier, ver, 32);
                    meta_save();
                    memcpy(vkey, newKey, 32);
                    send_event(ch, "AUTH", "PIN_UPDATED", NULL);
                } else {
                    send_error(ch, "AUTH", "UPDATE_FAILED", NULL);
                }
            }
        }
        memset(oldKey, 0, sizeof(oldKey));
        memset(newKey, 0, sizeof(newKey));
    } else if (strcmp(cmd, "FACTORY_RESET") == 0) {
        factory_reset();
        send_event(ch, "SYS", "FACTORY_RESET_COMPLETE", NULL);
    } else if (strcmp(cmd, "ENROLL_FINGER") == 0) {
        if (!fpga_pf_supported() || !fp_present()) {
            send_error(ch, "AUTH", "NO_SENSOR", "No fingerprint sensor is attached to this authenticator.");
        } else {
            send_event(ch, "SYS", "FP_ENROLL_START", NULL);
            fp_empty(); /* like the ESP32 build: one finger at a time */
            if (fp_enroll(1, 30000)) send_event(ch, "SYS", "FP_ENROLLED", NULL);
            else send_error(ch, "AUTH", "ENROLL_FAILED", "Fingerprint enrollment failed or timed out.");
        }
    } else if (strcmp(cmd, "DELETE_FINGERS") == 0) {
        if (fp_present() && fp_empty()) send_event(ch, "SYS", "FINGERS_DELETED", NULL);
        else send_error(ch, "AUTH", "NO_SENSOR", "No fingerprint sensor is attached to this authenticator.");
    } else if (strncmp(cmd, "BACKUP_", 7) == 0) {
        backup_command(ch, cmd, req);
    } else if (strcmp(cmd, "PROVISION_ATTESTATION") == 0) {
#ifdef POLKEY_ALLOW_ATT_PROVISIONING
        const char *privHex = jstr(req, "privkey", NULL, NULL), *certHex = jstr(req, "cert", NULL, NULL);
        static uint8_t cert[STORE_ATT_CERT_MAX];
        uint8_t priv[32];
        size_t cl = strlen(certHex) / 2;
        bool good = strlen(privHex) == 64 && cl > 0 && cl <= sizeof(cert) && strlen(certHex) % 2 == 0;
        for (size_t i = 0; good && i < 32; i++) {
            int a = hex_val(privHex[2 * i]), b = hex_val(privHex[2 * i + 1]);
            if (a < 0 || b < 0) good = false; else priv[i] = (uint8_t)(a << 4 | b);
        }
        for (size_t i = 0; good && i < cl; i++) {
            int a = hex_val(certHex[2 * i]), b = hex_val(certHex[2 * i + 1]);
            if (a < 0 || b < 0) good = false; else cert[i] = (uint8_t)(a << 4 | b);
        }
        const char *perr = "BAD_INPUT";
        if (good && ctap2_att_provision(priv, cert, cl, &perr)) send_event(ch, "FIDO2", "ATTESTATION_PROVISIONED", NULL);
        else send_error(ch, "FIDO2", "PROVISION_FAILED", perr);
        memset(priv, 0, sizeof(priv));
#else
        send_error(ch, "SYS", "NOT_SUPPORTED", "Attestation provisioning is disabled in this build.");
#endif
    } else {
        send_error(ch, "SYS", "UNKNOWN_CMD", NULL);
    }
    cJSON_Delete(req);
}
