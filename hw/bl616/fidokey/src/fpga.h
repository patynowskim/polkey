/*
 * FPGA crypto accelerator link for the PolKey FIDO2 authenticator (BL616)
 *
 * Transport: UART1 at 1.5 Mbaud (GPIO11 -> FPGA pin 70, GPIO13 <- FPGA pin 69),
 * the same framed protocol the host tools speak (hw/accel/host/tro_accel.py):
 *   A5 | cmd | len(2 LE) | payload | crc16(2 LE)        BL616 -> FPGA
 *   5A | status | len(2 LE) | payload | crc16(2 LE)     FPGA -> BL616
 * CRC-16/CCITT-FALSE over cmd..payload (response: status..payload).
 *
 * The asymmetric bitstream keeps P-256/Ed25519 private keys wrapped under the
 * device master key, so blobs are opaque 64-byte values.
 */
#ifndef FPGA_H
#define FPGA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FPGA_BLOB_LEN 64
#define FPGA_RSA_BLOB_LEN 672

#define FPGA_CAPS_SYM  0x00000007u /* SHA-256 / HMAC / PBKDF2 */
#define FPGA_CAPS_AES  0x00000008u /* AES-256 CBC / GCM */
#define FPGA_CAPS_PQ   0x00000300u
#define FPGA_CAPS_RSA  0x00000400u
#define FPGA_CAPS_KEM  0x00000800u

bool fpga_init(void);            /* UART + ISR only; the first call probes the FPGA */
bool fpga_available(void);       /* true if the accelerator answers (re-probes every few seconds) */
uint32_t fpga_caps(void);
bool fpga_has_asym(void);
bool fpga_has_sym(void);   /* SHA-256 / HMAC / PBKDF2 */
bool fpga_has_aes(void);   /* AES-256 CBC / GCM */
bool fpga_has_pq(void);
bool fpga_has_rsa(void);
bool fpga_has_kem(void);   /* ML-KEM-512/768/1024 */
bool fpga_uid(uint8_t uid16[16]);

bool fpga_fill_random(uint8_t *out, size_t len);

/* P-256: public key = X||Y (64 bytes big-endian), signature = r||s (64 bytes) */
bool fpga_p256_genkey(uint8_t blobOut[FPGA_BLOB_LEN], uint8_t pubOut64[64]);
bool fpga_p256_public(const uint8_t blob[FPGA_BLOB_LEN], uint8_t pubOut64[64]);
bool fpga_p256_sign(const uint8_t blob[FPGA_BLOB_LEN], const uint8_t digest32[32], uint8_t sigOut64[64]);

/* Ed25519: public key = 32 bytes, signature = 64 bytes over the raw message */
bool fpga_ed25519_genkey(uint8_t blobOut[FPGA_BLOB_LEN], uint8_t pubOut32[32]);
bool fpga_ed25519_public(const uint8_t blob[FPGA_BLOB_LEN], uint8_t pubOut32[32]);
bool fpga_ed25519_sign(const uint8_t blob[FPGA_BLOB_LEN], const uint8_t *msg, size_t len, uint8_t sigOut64[64]);

bool fpga_p256_ecdh(const uint8_t blob[FPGA_BLOB_LEN], const uint8_t peerPub64[64], uint8_t secretOut32[32]);
bool fpga_p256_verify(const uint8_t digest32[32], const uint8_t sig64[64], const uint8_t pub64[64]);
bool fpga_p256_import(const uint8_t raw32[32], uint8_t blobOut[FPGA_BLOB_LEN]);
bool fpga_ed25519_import(const uint8_t seed32[32], uint8_t blobOut[FPGA_BLOB_LEN]);

/* ML-DSA 44/65/87 (pq bitstream): blob = wrapped secret key + 32, public key 1312 / 1952 / 2592 bytes */
size_t fpga_mldsa_blob_len(int ps);
bool fpga_mldsa_genkey(int ps, uint8_t *blobOut, uint8_t *pubOut);
bool fpga_mldsa_sign(int ps, const uint8_t *blob, const uint8_t *msg, size_t msgLen, uint8_t *sigOut, size_t *sigLen);

/* ML-KEM (FIPS 203): level 0/1/2 = ML-KEM-512/768/1024. The decapsulation key lives in the FPGA-wrapped blob only.
 * sizes: ek = 384k + 32, ct = 32*(du*k + dv), blob = 768k + 128 (k = 2/3/4), shared secret = 32 bytes. */
bool fpga_kem_dims(int level, size_t *ek, size_t *ct, size_t *blob);
bool fpga_kem_keygen(int level, uint8_t *blobOut, uint8_t *ekOut);
bool fpga_kem_encaps(int level, const uint8_t *ek, uint8_t K32[32], uint8_t *ctOut);   /* status 3 from the FPGA = ek failed the modulus check */
bool fpga_kem_decaps(int level, const uint8_t *blob, const uint8_t *ct, uint8_t K32[32]);

/* RSA-2048 (e = 65537): keygen takes tens of seconds, signature is RSASSA-PKCS1-v1_5 / SHA-256 */
bool fpga_rsa_genkey(uint8_t blobOut[FPGA_RSA_BLOB_LEN], uint8_t nOut256[256]);
bool fpga_rsa_public(const uint8_t blob[FPGA_RSA_BLOB_LEN], uint8_t nOut256[256]);
bool fpga_rsa_sign_sha256(const uint8_t blob[FPGA_RSA_BLOB_LEN], const uint8_t digest32[32], uint8_t sigOut256[256]);

/* Hashing: SHA-512 (asym/main), SHA-256 / HMAC / PBKDF2 (sym) */
bool fpga_sha512(const uint8_t *data, size_t len, uint8_t out64[64]);
bool fpga_sha256(const uint8_t *data, size_t len, uint8_t out32[32]);
bool fpga_hmac_sha256(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t len, uint8_t out32[32]);
bool fpga_pbkdf2_sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt, size_t saltLen,
                        uint32_t iterations, uint8_t *out, size_t outLen);

/* AES-256 (sym bitstream), payload per call <= ~8000 bytes */
bool fpga_aes_set_key(const uint8_t key32[32]);
bool fpga_aes_cbc(bool encrypt, const uint8_t iv16[16], const uint8_t *in, size_t len, uint8_t *out);
bool fpga_aes_gcm_encrypt(const uint8_t iv12[12], const uint8_t *aad, size_t aadLen,
                          const uint8_t *in, size_t len, uint8_t *out, uint8_t tag16[16]);
bool fpga_aes_gcm_decrypt(const uint8_t iv12[12], const uint8_t *aad, size_t aadLen,
                          const uint8_t *in, size_t len, const uint8_t tag16[16], uint8_t *out);

/* User presence: button S2 of the Tang Nano and its LEDs (needs a bitstream with the ux unit) */
bool fpga_ux_supported(void);
bool fpga_ux_buttons(bool clear, uint8_t *state);   /* bit0 pressed, bit1 pressed since the last clearing read */
bool fpga_ux_led(uint8_t v);                        /* bits 0..2 force led0..2 on, bit 7 blinks them, 0 = status display */

/* Peripheral bridge (pf unit, commands 0x06 / 0x07): fingerprint sensor UART and SSD1306 OLED SPI, wired to FPGA pins */
bool fpga_pf_supported(void);
/* sends tx (57600 8N1), then collects up to maxRx bytes; ends after gapMs of silence, at maxRx or after timeoutMs */
bool fpga_fp_xfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t maxRx, size_t *got, uint8_t gapMs, uint16_t timeoutMs);
/* ctl bit0 = DC, bit1 = RES level, bit2 = apply RES; data is shifted out with CS low */
bool fpga_oled_write(uint8_t ctl, const uint8_t *data, size_t len);

/* While set, CTAPHID keepalives are sent every 100 ms during FPGA waits */
void fpga_set_keepalive(uint32_t channel);

#endif
