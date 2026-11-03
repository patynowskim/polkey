/*
 * Password vault + TOTP + management commands, port of the ESP32 JSON protocol (main.cpp / StorageManager.cpp)
 *
 * Transport: CTAPHID vendor command CTAPHID_VAULT carrying one UTF-8 JSON request and answering with one JSON
 * message in the same shape as the serial protocol: {"type":"event","module":..,"event":..,"data":{..}} or
 * {"type":"error","module":..,"error_code":..}. The BL616 has no spare USB endpoints for a CDC serial port.
 *
 * Vault entries are AES-256-GCM encrypted under a key derived from the vault PIN (PBKDF2-SHA256) and a secret
 * bound to this FPGA (ECDH with a wrapped device key), so the flash content alone is not enough to decrypt it.
 */
#ifndef VAULT_H
#define VAULT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CTAPHID_VAULT 0xC1 /* vendor command 0x41 */

void vault_init(void);                                           /* after store_init() */
void vault_handle(uint32_t channel, const uint8_t *data, size_t len);     /* CTAPHID vendor command */

/* Transport independent entry point: reply is called once with the JSON answer (task context) */
typedef void (*vault_reply_fn)(void *ctx, const uint8_t *msg, size_t len);
void vault_process(const uint8_t *data, size_t len, vault_reply_fn reply, void *ctx);
bool vault_unlocked(void);                                       /* the vault PIN was verified in this session */
bool vault_up_required(void);                                    /* user presence (button S2) is demanded */
int vault_default_alg(void);                                     /* COSE alg preferred for new credentials (0 = platform order) */

#endif
