/*
 * FPGA crypto accelerator link for the PolKey FIDO2 authenticator (BL616)
 */
#include <string.h>
#include "bflb_uart.h"
#include "bflb_irq.h"
#include "bflb_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ctaphid.h"
#include "fpga.h"

#define FPGA_UART_BAUD 1500000u
#define RING 16384u
#define FPGA_MAX_PAYLOAD 8000

enum {
    FPGA_OK = 0,
    FPGA_BAD_CMD = 1,
    FPGA_BAD_LEN = 2,
    FPGA_BAD_CRC = 3,
    FPGA_AUTH_FAIL = 4,
    FPGA_ERR = 5,
    FPGA_BAD_PARAM = 6,
    FPGA_NO_KEY = 7,
    FPGA_IO_TIMEOUT = 0xF0,
    FPGA_IO_CRC = 0xF1,
    FPGA_ABSENT = 0xF2,
};

#define FPGA_CAPS_ASYM 0x000000F0

static struct bflb_device_s *uart1_dev;
static uint8_t rxr[RING];
static volatile uint32_t rxr_head, rxr_tail;

static bool ready;
static bool ok;
static uint32_t caps;
static TickType_t retry_at;
static volatile uint32_t ka_channel;

static void uart_isr(int irq, void *arg)
{
    (void)irq;
    (void)arg;
    uint32_t st = bflb_uart_get_intstatus(uart1_dev);

    if (st & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) {
        uint32_t h = rxr_head;
        while (bflb_uart_rxavailable(uart1_dev)) {
            uint8_t c = (uint8_t)bflb_uart_getchar(uart1_dev);
            if (h - rxr_tail < RING) {
                rxr[h++ & (RING - 1)] = c;
            }
        }
        rxr_head = h;
        if (st & UART_INTSTS_RTO) {
            bflb_uart_int_clear(uart1_dev, UART_INTCLR_RTO);
        }
    }
}

static void keepalive_tick(TickType_t *last)
{
    if (ka_channel == 0) return;
    if ((xTaskGetTickCount() - *last) < pdMS_TO_TICKS(100)) return;
    uint8_t st = CTAPHID_STATUS_PROCESSING;
    ctaphid_send(ka_channel, CTAPHID_KEEPALIVE, &st, 1);
    *last = xTaskGetTickCount();
}

static void rx_flush(void)
{
    rxr_tail = rxr_head;
}

static bool rx_find_sof(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t last_ka = start;

    while ((xTaskGetTickCount() - start) <= pdMS_TO_TICKS(timeout_ms)) {
        while (rxr_tail != rxr_head) {
            uint8_t b = rxr[rxr_tail++ & (RING - 1)];
            if (b == 0x5A) return true;
        }
        keepalive_tick(&last_ka);
        vTaskDelay(1);
    }
    return false;
}

static bool rx_read(uint8_t *dst, size_t n, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t last_ka = start;
    size_t o = 0;

    while (o < n) {
        while (o < n && rxr_tail != rxr_head) {
            dst[o++] = rxr[rxr_tail++ & (RING - 1)];
        }
        if (o == n) break;
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) return false;
        keepalive_tick(&last_ka);
        vTaskDelay(1);
    }
    return true;
}

static bool tx_write(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        TickType_t t0 = xTaskGetTickCount();
        while (!bflb_uart_txready(uart1_dev)) {
            if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(100)) return false;
            vTaskDelay(1);
        }
        bflb_uart_putchar(uart1_dev, b[i]);
    }
    return true;
}

static uint16_t crc16(const uint8_t *d, size_t n, uint16_t c)
{
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static bool call_raw(uint8_t cmd, const uint8_t *pl, size_t plLen,
                     uint8_t *rsp, size_t rspMax, size_t *rspLen, uint32_t timeout_ms)
{
    if (!ready || plLen > FPGA_MAX_PAYLOAD) return false;

    uint8_t hdr[4] = { 0xA5, cmd, (uint8_t)(plLen & 0xFF), (uint8_t)(plLen >> 8) };
    uint16_t c = crc16(hdr + 1, 3, 0xFFFF);
    if (plLen) c = crc16(pl, plLen, c);
    uint8_t crcBytes[2] = { (uint8_t)(c & 0xFF), (uint8_t)(c >> 8) };

    rx_flush();
    if (!tx_write(hdr, 4) || (plLen && !tx_write(pl, plLen)) || !tx_write(crcBytes, 2)) {
        return false;
    }

    if (!rx_find_sof(timeout_ms)) return false;

    uint8_t h[3];
    if (!rx_read(h, 3, timeout_ms)) return false;
    size_t n = (size_t)h[1] | ((size_t)h[2] << 8);
    if (n > rspMax) return false;
    if (n && !rx_read(rsp, n, timeout_ms)) return false;

    uint8_t cc[2];
    if (!rx_read(cc, 2, timeout_ms)) return false;

    uint16_t rc = crc16(h, 3, 0xFFFF);
    rc = crc16(rsp, n, rc);
    if (rc != ((uint16_t)cc[0] | ((uint16_t)cc[1] << 8))) return false;

    if (rspLen) *rspLen = n;
    return h[0] == FPGA_OK;
}

bool fpga_init(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    bflb_gpio_uart_init(gpio, GPIO_PIN_11, GPIO_UART_FUNC_UART1_TX);
    bflb_gpio_uart_init(gpio, GPIO_PIN_13, GPIO_UART_FUNC_UART1_RX);

    uart1_dev = bflb_device_get_by_name("uart1");

    struct bflb_uart_config_s cfg = {
        .baudrate = FPGA_UART_BAUD,
        .direction = UART_DIRECTION_TXRX,
        .data_bits = UART_DATA_BITS_8,
        .stop_bits = UART_STOP_BITS_1,
        .parity = UART_PARITY_NONE,
        .bit_order = UART_LSB_FIRST,
        .flow_ctrl = 0,
        .tx_fifo_threshold = 7,
        .rx_fifo_threshold = 7,
    };
    bflb_irq_attach(uart1_dev->irq_num, uart_isr, NULL);
    bflb_uart_init(uart1_dev, &cfg);
    bflb_uart_rxint_mask(uart1_dev, false);
    bflb_irq_enable(uart1_dev->irq_num);

    ready = true;
    return true;
}

bool fpga_available(void)
{
    if (!ready) return false;
    if (ok) return true;
    if (xTaskGetTickCount() < retry_at) return false;

    uint8_t r[12];
    size_t n = 0;
    if (call_raw(0x01, NULL, 0, r, sizeof(r), &n, 100) && n == 12 && memcmp(r, "TROACC01", 8) == 0) {
        caps = ((uint32_t)r[8] << 24) | ((uint32_t)r[9] << 16) | ((uint32_t)r[10] << 8) | r[11];
        ok = true;
        return true;
    }

    retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    return false;
}

uint32_t fpga_caps(void)
{
    return fpga_available() ? caps : 0;
}

bool fpga_has_asym(void)
{
    return (fpga_caps() & FPGA_CAPS_ASYM) != 0;
}

bool fpga_fill_random(uint8_t *out, size_t len)
{
    if (!fpga_available()) return false;
    while (len) {
        uint8_t r[32];
        if (!call_raw(0x03, NULL, 0, r, sizeof(r), NULL, 3000)) return false;
        size_t n = len < 32 ? len : 32;
        memcpy(out, r, n);
        out += n;
        len -= n;
    }
    return true;
}

static bool call_exact(uint8_t cmd, const uint8_t *pl, size_t plLen, uint8_t *rsp, size_t want, uint32_t timeout_ms)
{
    size_t n = 0;
    return call_raw(cmd, pl, plLen, rsp, want, &n, timeout_ms) && n == want;
}

bool fpga_p256_genkey(uint8_t blobOut[FPGA_BLOB_LEN], uint8_t pubOut64[64])
{
    if (!fpga_has_asym()) return false;
    uint8_t r[128];
    if (!call_exact(0x30, NULL, 0, r, sizeof(r), 5000)) return false;
    memcpy(blobOut, r, FPGA_BLOB_LEN);
    memcpy(pubOut64, r + FPGA_BLOB_LEN, 64);
    return true;
}

bool fpga_p256_public(const uint8_t blob[FPGA_BLOB_LEN], uint8_t pubOut64[64])
{
    return fpga_has_asym() && call_exact(0x31, blob, FPGA_BLOB_LEN, pubOut64, 64, 3000);
}

bool fpga_p256_sign(const uint8_t blob[FPGA_BLOB_LEN], const uint8_t digest32[32], uint8_t sigOut64[64])
{
    if (!fpga_has_asym()) return false;
    uint8_t p[96];
    memcpy(p, blob, FPGA_BLOB_LEN);
    memcpy(p + FPGA_BLOB_LEN, digest32, 32);
    return call_exact(0x32, p, sizeof(p), sigOut64, 64, 5000);
}

bool fpga_ed25519_genkey(uint8_t blobOut[FPGA_BLOB_LEN], uint8_t pubOut32[32])
{
    if (!fpga_has_asym()) return false;
    uint8_t r[96];
    if (!call_exact(0x40, NULL, 0, r, sizeof(r), 5000)) return false;
    memcpy(blobOut, r, FPGA_BLOB_LEN);
    memcpy(pubOut32, r + FPGA_BLOB_LEN, 32);
    return true;
}

bool fpga_ed25519_public(const uint8_t blob[FPGA_BLOB_LEN], uint8_t pubOut32[32])
{
    return fpga_has_asym() && call_exact(0x41, blob, FPGA_BLOB_LEN, pubOut32, 32, 3000);
}

static uint8_t scratch[FPGA_MAX_PAYLOAD + 64]; /* request/response staging; the FPGA link is used by the CTAPHID task only */

bool fpga_ed25519_sign(const uint8_t blob[FPGA_BLOB_LEN], const uint8_t *msg, size_t len, uint8_t sigOut64[64])
{
    if (!fpga_has_asym() || len + FPGA_BLOB_LEN > FPGA_MAX_PAYLOAD) return false;
    memcpy(scratch, blob, FPGA_BLOB_LEN);
    if (len) memcpy(scratch + FPGA_BLOB_LEN, msg, len);
    bool ok = call_exact(0x42, scratch, len + FPGA_BLOB_LEN, sigOut64, 64, 5000);
    memset(scratch, 0, FPGA_BLOB_LEN);
    return ok;
}

bool fpga_ed25519_import(const uint8_t seed32[32], uint8_t blobOut[FPGA_BLOB_LEN])
{
    return fpga_has_asym() && call_exact(0x43, seed32, 32, blobOut, FPGA_BLOB_LEN, 3000);
}

bool fpga_p256_ecdh(const uint8_t blob[FPGA_BLOB_LEN], const uint8_t peerPub64[64], uint8_t secretOut32[32])
{
    if (!fpga_has_asym()) return false;
    uint8_t p[128];
    memcpy(p, blob, FPGA_BLOB_LEN);
    memcpy(p + FPGA_BLOB_LEN, peerPub64, 64);
    return call_exact(0x33, p, sizeof(p), secretOut32, 32, 5000);
}

bool fpga_p256_verify(const uint8_t digest32[32], const uint8_t sig64[64], const uint8_t pub64[64])
{
    if (!fpga_has_asym()) return false;
    uint8_t p[160];
    memcpy(p, digest32, 32);
    memcpy(p + 32, sig64, 64);
    memcpy(p + 96, pub64, 64);
    size_t n = 0;
    return call_raw(0x34, p, sizeof(p), NULL, 0, &n, 6000) && n == 0;
}

bool fpga_p256_import(const uint8_t raw32[32], uint8_t blobOut[FPGA_BLOB_LEN])
{
    return fpga_has_asym() && call_exact(0x35, raw32, 32, blobOut, FPGA_BLOB_LEN, 3000);
}

bool fpga_uid(uint8_t uid16[16])
{
    return fpga_available() && call_exact(0x02, NULL, 0, uid16, 16, 1000);
}

/* ---------------- ML-DSA ---------------- */
static bool mldsa_dims(int ps, size_t *sk, size_t *pk, size_t *sig, uint8_t *idx)
{
    switch (ps) {
    case 44: *sk = 2560; *pk = 1312; *sig = 2420; *idx = 0; return true;
    case 65: *sk = 4032; *pk = 1952; *sig = 3309; *idx = 1; return true;
    case 87: *sk = 4896; *pk = 2592; *sig = 4627; *idx = 2; return true;
    }
    return false;
}

size_t fpga_mldsa_blob_len(int ps)
{
    size_t sk, pk, sig;
    uint8_t i;
    return mldsa_dims(ps, &sk, &pk, &sig, &i) ? sk + 32 : 0;
}

bool fpga_has_pq(void)
{
    return (fpga_caps() & FPGA_CAPS_PQ) != 0;
}

bool fpga_has_rsa(void)
{
    return (fpga_caps() & FPGA_CAPS_RSA) != 0;
}

bool fpga_mldsa_genkey(int ps, uint8_t *blobOut, uint8_t *pubOut)
{
    size_t sk, pk, sig;
    uint8_t idx;
    if (!fpga_has_pq() || !mldsa_dims(ps, &sk, &pk, &sig, &idx)) return false;
    size_t total = sk + 32 + pk;
    bool ok = call_exact(0x60 + idx, NULL, 0, scratch, total, 8000);
    if (ok) {
        memcpy(blobOut, scratch, sk + 32);
        memcpy(pubOut, scratch + sk + 32, pk);
    }
    memset(scratch, 0, total);
    return ok;
}

bool fpga_mldsa_sign(int ps, const uint8_t *blob, const uint8_t *msg, size_t msgLen, uint8_t *sigOut, size_t *sigLen)
{
    size_t sk, pk, sig;
    uint8_t idx;
    if (!fpga_has_pq() || !mldsa_dims(ps, &sk, &pk, &sig, &idx)) return false;
    size_t bl = sk + 32, total = bl + 32 + msgLen;
    if (total > FPGA_MAX_PAYLOAD) return false;
    memcpy(scratch, blob, bl);
    memset(scratch + bl, 0, 32); /* hedge randomness comes from the FPGA TRNG */
    if (msgLen) memcpy(scratch + bl + 32, msg, msgLen);
    bool ok = call_exact(0x68 + idx, scratch, total, sigOut, sig, 30000);
    memset(scratch, 0, total);
    if (ok && sigLen) *sigLen = sig;
    return ok;
}

/* ---------------- ML-KEM ---------------- */
bool fpga_has_kem(void)
{
    return (fpga_caps() & FPGA_CAPS_KEM) != 0;
}

bool fpga_kem_dims(int level, size_t *ek, size_t *ct, size_t *blob)
{
    static const uint8_t k[3] = { 2, 3, 4 }, du[3] = { 10, 10, 11 }, dv[3] = { 4, 4, 5 };
    if (level < 0 || level > 2) return false;
    *ek = 384u * k[level] + 32;
    *ct = 32u * (du[level] * k[level] + dv[level]);
    *blob = 768u * k[level] + 128;
    return true;
}

bool fpga_kem_keygen(int level, uint8_t *blobOut, uint8_t *ekOut)
{
    size_t ek, ct, bl;
    if (!fpga_has_kem() || !fpga_kem_dims(level, &ek, &ct, &bl)) return false;
    bool ok = call_exact(0x80 + level, NULL, 0, scratch, bl + ek, 20000);
    if (ok) {
        memcpy(blobOut, scratch, bl);
        memcpy(ekOut, scratch + bl, ek);
    }
    memset(scratch, 0, bl + ek);
    return ok;
}

bool fpga_kem_encaps(int level, const uint8_t *ek, uint8_t K32[32], uint8_t *ctOut)
{
    size_t eklen, ct, bl;
    if (!fpga_has_kem() || !fpga_kem_dims(level, &eklen, &ct, &bl)) return false;
    memcpy(scratch, ek, eklen);
    bool ok = call_exact(0x88 + level, scratch, eklen, scratch, 32 + ct, 20000);
    if (ok) {
        memcpy(K32, scratch, 32);
        memcpy(ctOut, scratch + 32, ct);
    }
    memset(scratch, 0, 32 + ct);
    return ok;
}

bool fpga_kem_decaps(int level, const uint8_t *blob, const uint8_t *ct, uint8_t K32[32])
{
    size_t ek, ctlen, bl;
    if (!fpga_has_kem() || !fpga_kem_dims(level, &ek, &ctlen, &bl) || bl + ctlen > FPGA_MAX_PAYLOAD) return false;
    memcpy(scratch, blob, bl);
    memcpy(scratch + bl, ct, ctlen);
    bool ok = call_exact(0x90 + level, scratch, bl + ctlen, scratch, 32, 30000);
    if (ok) memcpy(K32, scratch, 32);
    memset(scratch, 0, bl + ctlen);
    return ok;
}

/* ---------------- RSA-2048 ---------------- */
bool fpga_rsa_genkey(uint8_t blobOut[FPGA_RSA_BLOB_LEN], uint8_t nOut256[256])
{
    if (!fpga_has_rsa()) return false;
    bool ok = call_exact(0x70, NULL, 0, scratch, FPGA_RSA_BLOB_LEN + 256, 600000);
    if (ok) {
        memcpy(blobOut, scratch, FPGA_RSA_BLOB_LEN);
        memcpy(nOut256, scratch + FPGA_RSA_BLOB_LEN, 256);
    }
    memset(scratch, 0, FPGA_RSA_BLOB_LEN + 256);
    return ok;
}

bool fpga_rsa_public(const uint8_t blob[FPGA_RSA_BLOB_LEN], uint8_t nOut256[256])
{
    return fpga_has_rsa() && call_exact(0x71, blob, FPGA_RSA_BLOB_LEN, nOut256, 256, 10000);
}

bool fpga_rsa_sign_sha256(const uint8_t blob[FPGA_RSA_BLOB_LEN], const uint8_t digest32[32], uint8_t sigOut256[256])
{
    if (!fpga_has_rsa()) return false;
    uint8_t p[FPGA_RSA_BLOB_LEN + 32];
    memcpy(p, blob, FPGA_RSA_BLOB_LEN);
    memcpy(p + FPGA_RSA_BLOB_LEN, digest32, 32);
    bool ok = call_exact(0x72, p, sizeof(p), sigOut256, 256, 30000);
    memset(p, 0, sizeof(p));
    return ok;
}

/* ---------------- hashing (SHA-512 asym; SHA-256/HMAC/PBKDF2 sym) ---------------- */
bool fpga_has_sym(void)
{
    return (fpga_caps() & FPGA_CAPS_SYM) != 0;
}

bool fpga_has_aes(void)
{
    return (fpga_caps() & FPGA_CAPS_AES) != 0;
}

bool fpga_sha512(const uint8_t *data, size_t len, uint8_t out64[64])
{
    return fpga_has_asym() && len <= FPGA_MAX_PAYLOAD && call_exact(0x50, data, len, out64, 64, 5000);
}

bool fpga_sha256(const uint8_t *data, size_t len, uint8_t out32[32])
{
    return fpga_has_sym() && len <= FPGA_MAX_PAYLOAD && call_exact(0x10, data, len, out32, 32, 5000);
}

bool fpga_hmac_sha256(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t len, uint8_t out32[32])
{
    if (!fpga_has_sym() || keyLen > 64 || len + 1 + keyLen > FPGA_MAX_PAYLOAD) return false;
    scratch[0] = (uint8_t)keyLen;
    memcpy(scratch + 1, key, keyLen);
    if (len) memcpy(scratch + 1 + keyLen, data, len);
    bool ok = call_exact(0x11, scratch, 1 + keyLen + len, out32, 32, 5000);
    memset(scratch, 0, 1 + keyLen);
    return ok;
}

bool fpga_pbkdf2_sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt, size_t saltLen,
                        uint32_t iterations, uint8_t *out, size_t outLen)
{
    if (!fpga_has_sym() || pwLen > 64 || outLen == 0 || outLen > 1024 || iterations == 0 ||
        7 + pwLen + saltLen > FPGA_MAX_PAYLOAD) return false;
    scratch[0] = (uint8_t)iterations;
    scratch[1] = (uint8_t)(iterations >> 8);
    scratch[2] = (uint8_t)(iterations >> 16);
    scratch[3] = (uint8_t)(iterations >> 24);
    scratch[4] = (uint8_t)(outLen & 0xFF);
    scratch[5] = (uint8_t)(outLen >> 8);
    scratch[6] = (uint8_t)pwLen;
    memcpy(scratch + 7, pw, pwLen);
    if (saltLen) memcpy(scratch + 7 + pwLen, salt, saltLen);
    /* ~136 clock cycles per iteration at 27 MHz (5 us), per 32-byte output block */
    uint32_t blocks = (uint32_t)((outLen + 31) / 32);
    uint32_t t = 3000 + blocks * (uint32_t)((uint64_t)iterations * 6 / 1000 + 50);
    bool ok = call_exact(0x12, scratch, 7 + pwLen + saltLen, out, outLen, t);
    memset(scratch, 0, 7 + pwLen);
    return ok;
}

/* ---------------- AES-256 (sym) ---------------- */
bool fpga_aes_set_key(const uint8_t key32[32])
{
    size_t n = 0;
    return fpga_has_aes() && call_raw(0x20, key32, 32, NULL, 0, &n, 1000);
}

static bool aes_op(uint8_t mode, const uint8_t *iv, size_t ivLen, const uint8_t *aad, size_t aadLen,
                   const uint8_t *in, size_t len, const uint8_t *tagIn, uint8_t *out, uint8_t *tagOut)
{
    if (!fpga_has_aes()) return false;
    size_t total = 21 + aadLen + len + (tagIn ? 16 : 0);
    if (total > FPGA_MAX_PAYLOAD || len + 16 > sizeof(scratch)) return false;
    static uint8_t rbuf[FPGA_MAX_PAYLOAD + 16];
    uint8_t *p = scratch;
    p[0] = mode;
    memset(p + 1, 0, 16);
    memcpy(p + 1, iv, ivLen);
    p[17] = (uint8_t)(aadLen & 0xFF);
    p[18] = (uint8_t)(aadLen >> 8);
    p[19] = (uint8_t)(len & 0xFF);
    p[20] = (uint8_t)(len >> 8);
    size_t o = 21;
    if (aadLen) { memcpy(p + o, aad, aadLen); o += aadLen; }
    if (len) { memcpy(p + o, in, len); o += len; }
    if (tagIn) memcpy(p + o, tagIn, 16);
    size_t want = len + ((mode == 4) ? 16 : 0), n = 0;
    bool ok = call_raw(0x21, p, total, rbuf, len + 16, &n, 5000) && n == want;
    if (ok) {
        if (len) memcpy(out, rbuf, len);
        if (tagOut) memcpy(tagOut, rbuf + len, 16);
    }
    return ok;
}

bool fpga_aes_cbc(bool encrypt, const uint8_t iv16[16], const uint8_t *in, size_t len, uint8_t *out)
{
    return aes_op(encrypt ? 2 : 3, iv16, 16, NULL, 0, in, len, NULL, out, NULL);
}

bool fpga_aes_gcm_encrypt(const uint8_t iv12[12], const uint8_t *aad, size_t aadLen,
                          const uint8_t *in, size_t len, uint8_t *out, uint8_t tag16[16])
{
    return aes_op(4, iv12, 12, aad, aadLen, in, len, NULL, out, tag16);
}

bool fpga_aes_gcm_decrypt(const uint8_t iv12[12], const uint8_t *aad, size_t aadLen,
                          const uint8_t *in, size_t len, const uint8_t tag16[16], uint8_t *out)
{
    return aes_op(5, iv12, 12, aad, aadLen, in, len, tag16, out, NULL);
}

/* ---------------- user presence button (S2) and LEDs: commands 0x04 / 0x05 of the bitstream ---------------- */
static enum { UX_UNKNOWN, UX_YES, UX_NO } ux_state;
static TickType_t ux_retry_at;

bool fpga_ux_supported(void)
{
    if (!fpga_available()) return false;
    if (ux_state == UX_YES) return true;
    if (ux_state == UX_NO && xTaskGetTickCount() < ux_retry_at) return false;
    uint8_t zero = 0, r[1];
    size_t n = 0;
    if (call_raw(0x04, &zero, 1, r, sizeof(r), &n, 200) && n == 1) {
        ux_state = UX_YES;
        return true;
    }
    ux_state = UX_NO; /* older bitstream: BAD_CMD (the bitstream may be replaced at run time, so look again later) */
    ux_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    return false;
}

/* bit0 = pressed now, bit1 = pressed since the last clearing read (latch expires after 3 s) */
bool fpga_ux_buttons(bool clear, uint8_t *state)
{
    uint8_t mode = clear ? 1 : 0, r[1];
    size_t n = 0;
    if (!call_raw(0x04, &mode, 1, r, sizeof(r), &n, 200) || n != 1) return false;
    *state = r[0];
    return true;
}

bool fpga_ux_led(uint8_t v)
{
    size_t n = 0;
    return call_raw(0x05, &v, 1, NULL, 0, &n, 200);
}

/* ---------------- peripheral bridge: fingerprint UART (0x06) and OLED SPI (0x07) ---------------- */
static enum { PF_UNKNOWN, PF_YES, PF_NO } pf_state;
static TickType_t pf_retry_at;

bool fpga_pf_supported(void)
{
    if (!fpga_available()) return false;
    if (pf_state == PF_YES) return true;
    if (pf_state == PF_NO && xTaskGetTickCount() < pf_retry_at) return false;
    const uint8_t probe[5] = { 0, 0, 0, 0, 0 }; /* max_rx = 0: answers at once */
    size_t n = 0;
    if (call_raw(0x06, probe, sizeof(probe), NULL, 0, &n, 200)) {
        pf_state = PF_YES;
        return true;
    }
    pf_state = PF_NO;
    pf_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    return false;
}

bool fpga_fp_xfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t maxRx, size_t *got, uint8_t gapMs, uint16_t timeoutMs)
{
    static uint8_t buf[5 + 64];
    if (txLen > 64 || maxRx > 0xFFFF) return false;
    buf[0] = gapMs;
    buf[1] = (uint8_t)(timeoutMs & 0xFF);
    buf[2] = (uint8_t)(timeoutMs >> 8);
    buf[3] = (uint8_t)(maxRx & 0xFF);
    buf[4] = (uint8_t)(maxRx >> 8);
    if (txLen) memcpy(buf + 5, tx, txLen);
    size_t n = 0;
    if (!call_raw(0x06, buf, 5 + txLen, rx, maxRx, &n, (uint32_t)timeoutMs + 200)) return false;
    if (got) *got = n;
    return true;
}

bool fpga_oled_write(uint8_t ctl, const uint8_t *data, size_t len)
{
    static uint8_t buf[1 + 520];
    if (len > 520) return false;
    buf[0] = ctl;
    if (len) memcpy(buf + 1, data, len);
    size_t n = 0;
    return call_raw(0x07, buf, 1 + len, NULL, 0, &n, 300);
}

void fpga_set_keepalive(uint32_t channel)
{
    ka_channel = channel;
}
