#include <string.h>
#include "kem_cmd.h"
#include "ctaphid.h"
#include "fpga.h"
#include "vault.h"

/* worst case ML-KEM-1024: blob 3200 + ek 1568, K + ct = 1600 */
static uint8_t kb[1 + 3200 + 1568];
static uint8_t kin[3200 + 1568];

static void reply(uint32_t ch, uint8_t status, size_t n)
{
    kb[0] = status;
    ctaphid_send(ch, CTAPHID_KEM, kb, (uint16_t)(1 + (status ? 0 : n)));
}

void kem_handle(uint32_t ch, const uint8_t *d, size_t len)
{
    size_t ek, ct, bl;
    if (len < 2) { reply(ch, 2, 0); return; }
    uint8_t op = d[0]; int lvl = d[1];
    if (!vault_unlocked()) { reply(ch, 5, 0); return; }
    if (!fpga_available() || !fpga_has_kem()) { reply(ch, 1, 0); return; }
    if (!fpga_kem_dims(lvl, &ek, &ct, &bl)) { reply(ch, 2, 0); return; }
    d += 2; len -= 2;
    if (op == 1 && len == 0) {
        reply(ch, fpga_kem_keygen(lvl, kb + 1, kb + 1 + bl) ? 0 : 3, bl + ek);
    } else if (op == 2 && len == ek) {
        memcpy(kin, d, len);
        reply(ch, fpga_kem_encaps(lvl, kin, kb + 1, kb + 1 + 32) ? 0 : 3, 32 + ct);
    } else if (op == 3 && len == bl + ct) {
        memcpy(kin, d, len);
        reply(ch, fpga_kem_decaps(lvl, kin, kin + bl, kb + 1) ? 0 : 3, 32);
    } else {
        reply(ch, 2, 0);
    }
    memset(kin, 0, sizeof(kin));
}
