/* Synochip R30x fingerprint protocol (same module family the ESP32 build drives through Adafruit_Fingerprint) */
#include "fp.h"
#include "fpga.h"
#include "oled.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define PID_CMD 0x01
#define PID_ACK 0x07

static bool present;
static TickType_t retry_at;
static int enrolled_cache = -1;

/* one command/acknowledge round trip; returns the confirmation code (0 = ok) or -1 on a transport / framing error */
static int fp_cmd(const uint8_t *payload, size_t n, uint8_t *ack, size_t ackMax, size_t *ackLen, uint16_t timeout_ms)
{
    uint8_t tx[40], rx[40];
    size_t len = n + 2, k = 0, got = 0;
    if (n + 11 > sizeof(tx)) return -1;
    tx[k++] = 0xEF; tx[k++] = 0x01;
    tx[k++] = 0xFF; tx[k++] = 0xFF; tx[k++] = 0xFF; tx[k++] = 0xFF;
    tx[k++] = PID_CMD;
    tx[k++] = (uint8_t)(len >> 8); tx[k++] = (uint8_t)len;
    memcpy(tx + k, payload, n); k += n;
    uint16_t sum = PID_CMD + (uint16_t)(len >> 8) + (uint16_t)(len & 0xFF);
    for (size_t i = 0; i < n; i++) sum += payload[i];
    tx[k++] = (uint8_t)(sum >> 8); tx[k++] = (uint8_t)sum;

    if (!fpga_fp_xfer(tx, k, rx, sizeof(rx), &got, 4, timeout_ms)) return -1;
    if (got < 12 || rx[0] != 0xEF || rx[1] != 0x01 || rx[6] != PID_ACK) return -1;
    size_t alen = ((size_t)rx[7] << 8) | rx[8];
    if (alen < 3 || 9 + alen > got) return -1;
    uint16_t s = rx[6] + (uint16_t)rx[7] + (uint16_t)rx[8];
    for (size_t i = 0; i < alen - 2; i++) s += rx[9 + i];
    if (s != (uint16_t)(((uint16_t)rx[9 + alen - 2] << 8) | rx[9 + alen - 1])) return -1;
    size_t dl = alen - 2;                   /* confirmation code + data */
    if (ack && dl - 1 <= ackMax) { memcpy(ack, rx + 10, dl - 1); if (ackLen) *ackLen = dl - 1; }
    return rx[9];
}

static int simple(uint8_t c) { return fp_cmd(&c, 1, NULL, 0, NULL, 400); }

bool fp_present(void)
{
    if (present) return true;
    if (!fpga_pf_supported()) return false;
    if (retry_at && (int32_t)(xTaskGetTickCount() - retry_at) < 0) return false;
    const uint8_t vfy[5] = { 0x13, 0, 0, 0, 0 };          /* VfyPwd with the default password */
    if (fp_cmd(vfy, sizeof(vfy), NULL, 0, NULL, 300) == 0) {
        present = true;
        enrolled_cache = -1;
        return true;
    }
    retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    return false;
}

int fp_enrolled(void)
{
    if (!fp_present()) return -1;
    if (enrolled_cache >= 0) return enrolled_cache;
    uint8_t c = 0x1D, a[2];
    size_t al = 0;
    if (fp_cmd(&c, 1, a, sizeof(a), &al, 400) != 0 || al != 2) return -1;
    enrolled_cache = ((int)a[0] << 8) | a[1];
    return enrolled_cache;
}

static int gen_image(void) { return simple(0x01); }                 /* 0 ok, 2 no finger */
static int img2tz(uint8_t slot) { uint8_t p[2] = { 0x02, slot }; return fp_cmd(p, 2, NULL, 0, NULL, 500); }

int fp_poll(uint16_t *score)
{
    if (!fp_present()) return FP_ERR;
    int r = gen_image();
    if (r == 2) return FP_NOFINGER;
    if (r != 0) { if (r < 0) present = false; return FP_ERR; }
    if (img2tz(1) != 0) return FP_ERR;
    uint8_t p[6] = { 0x04, 0x01, 0x00, 0x00, 0x00, 0xA3 }, a[4];
    size_t al = 0;
    r = fp_cmd(p, sizeof(p), a, sizeof(a), &al, 800);
    if (r == 9) return FP_NOMATCH;                                  /* no matching template */
    if (r != 0 || al != 4) return FP_ERR;
    uint16_t sc = ((uint16_t)a[2] << 8) | a[3];
    if (score) *score = sc;
    return sc > FP_MIN_SCORE ? FP_MATCH : FP_NOMATCH;
}

bool fp_empty(void)
{
    if (!fp_present()) return false;
    enrolled_cache = -1;
    return simple(0x0D) == 0;
}

static bool wait_image(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        int r = gen_image();
        if (r == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

bool fp_enroll(uint16_t id, uint32_t timeout_ms)
{
    if (!fp_present()) return false;
    oled_show("Place finger", "");
    if (!wait_image(timeout_ms) || img2tz(1) != 0) return false;
    oled_show("Remove finger", "");
    vTaskDelay(pdMS_TO_TICKS(1200));
    TickType_t start = xTaskGetTickCount();
    while (gen_image() != 2) {                                      /* wait until the finger is gone */
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    oled_show("Place SAME", "finger again");
    if (!wait_image(timeout_ms) || img2tz(2) != 0) return false;
    if (simple(0x05) != 0) { oled_show("MISMATCH!", ""); return false; }   /* RegModel */
    uint8_t st[4] = { 0x06, 0x01, (uint8_t)(id >> 8), (uint8_t)id };
    bool ok = fp_cmd(st, sizeof(st), NULL, 0, NULL, 800) == 0;
    enrolled_cache = -1;
    oled_show(ok ? "STORED OK" : "STORE FAILED", "");
    return ok;
}
