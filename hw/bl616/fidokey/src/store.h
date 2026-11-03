/*
 * Credential store for the PolKey FIDO2 authenticator (BL616)
 *
 * Fixed 1 KB slots in the last 64 KB of flash. Each resident credential keeps
 * the FPGA-wrapped private key blob; the key itself never leaves the FPGA.
 */
#ifndef STORE_H
#define STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define STORE_CRED_ID_LEN 16
#define STORE_BLOB_MAX 64
#define STORE_USER_ID_MAX 64
#define STORE_RP_ID_MAX 128
#define STORE_USER_NAME_MAX 64
#define STORE_MAX_CRED 64

#define STORE_FLAG_HMAC_SECRET 0x01
#define STORE_FLAG_LBKEY 0x02   /* has a largeBlobKey */
#define STORE_FLAG_U2F 0x04     /* U2F key handle: not discoverable, not listed */
#define STORE_CREDBLOB_MAX 32

typedef struct {
    uint8_t rpIdHash[32];
    uint8_t credId[STORE_CRED_ID_LEN];
    uint8_t userId[STORE_USER_ID_MAX];
    uint8_t userIdLen;
    int16_t alg; /* COSE algorithm */
    uint8_t blob[STORE_BLOB_MAX];
    uint16_t blobLen;       /* wrapped key length; > STORE_BLOB_MAX means it lives in a big slot */
    uint16_t pubLen;        /* public key kept after the wrapped key in the big slot (0 if not stored) */
    uint8_t credBlobLen;
    uint8_t credBlob[STORE_CREDBLOB_MAX];
    uint8_t lbKey[32];
    uint8_t bigSlot;        /* 0 = inline blob, n+1 = big slot n */
    uint32_t signCount;
    uint8_t credRandom[64]; /* hmac-secret: [0..31] without UV, [32..63] with UV */
    uint8_t flags;          /* STORE_FLAG_* */
    uint8_t credProtect;    /* 0 = unset, 1..3 */
    char rpId[STORE_RP_ID_MAX];
    char userName[STORE_USER_NAME_MAX];
} cred_record_t;

bool store_init(void);

/* All functions below run in the CTAPHID task only. */
int store_count(void);
bool store_add(const cred_record_t *rec);                       /* replaces same rp+user */
/* Same, plus a large wrapped key (RSA / ML-DSA) kept in its own flash slot; rec->blobLen/bigSlot are set here */
#define STORE_BIG_MAX 7680 /* wrapped key + public key must fit one 8 KB big slot */
bool store_add_big(cred_record_t *rec, const uint8_t *big, size_t bigLen); /* sets rec->bigSlot; caller sets blobLen/pubLen */
bool store_big_read(const cred_record_t *rec, uint8_t *out, size_t cap); /* reads rec->blobLen bytes */
bool store_big_read_pub(const cred_record_t *rec, uint8_t *out, size_t cap); /* reads rec->pubLen bytes */

/* Iteration over all valid slots: returns the index of the next valid slot >= idx (filling *out) or -1 */
int store_iter(int idx, cred_record_t *out);
bool store_delete_by_credid(const uint8_t id[STORE_CRED_ID_LEN]);
bool store_update_user(const uint8_t id[STORE_CRED_ID_LEN], const uint8_t *userId, size_t userIdLen, const char *userName);
int store_capacity(void);

/* Serialized large-blob array (authenticatorLargeBlobs), at most STORE_LB_MAX bytes */
#define STORE_LB_MAX 4096
bool store_lb_read(uint8_t *out, size_t cap, size_t *len); /* false: nothing stored yet */
bool store_lb_write(const uint8_t *data, size_t len);

/* Password vault area: 1 metadata sector + 8 entry sectors (see vault.c) */
#define STORE_VAULT_SECTORS 9
bool store_vault_read(uint32_t off, uint8_t *out, size_t n);
bool store_vault_write_sector(unsigned sector, const uint8_t *data4096);
void store_vault_erase(void);

/* U2F attestation key (FPGA-wrapped P-256), created on first use */
#define STORE_ATT_CERT_MAX 1024
bool store_att_load(uint8_t blob[64], uint8_t pub[64], uint8_t *cert, size_t *certLen);
bool store_att_save(const uint8_t blob[64], const uint8_t pub[64], const uint8_t *cert, size_t certLen);
bool store_find_by_credid(const uint8_t id[STORE_CRED_ID_LEN], cred_record_t *out);
bool store_find_by_rp(const uint8_t rpIdHash[32], cred_record_t *out);
int store_count_by_rp(const uint8_t rpIdHash[32]);
bool store_find_by_rp_nth(const uint8_t rpIdHash[32], int n, cred_record_t *out); /* n-th match (0-based) */
bool store_update_sign_count(const uint8_t credId[STORE_CRED_ID_LEN], uint32_t signCount);
void store_wipe(void);

/* Device metadata (PIN) in its own sector below the credential region */
typedef struct {
    uint8_t pinSet;
    uint8_t retries;          /* remaining PIN attempts */
    uint8_t pinSalt[16];
    uint8_t pinVerifier[32];  /* SHA-256(salt || LEFT(SHA-256(pin), 16)) */
} store_meta_t;

bool store_meta_load(store_meta_t *m);   /* false -> defaults (no PIN, 8 retries) */
bool store_meta_save(const store_meta_t *m);

#endif
