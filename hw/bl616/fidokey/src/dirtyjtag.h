#ifndef DIRTYJTAG_H
#define DIRTYJTAG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void dirtyjtag_init(uint8_t busid);
void dirtyjtag_start(void);

extern volatile uint32_t polkey_stage;
#define STAGE(n) (polkey_stage = (n))

/* JSON tunnel (command 0xF0) used by the browser extension via WebUSB */
bool dirtyjtag_vault_take(const uint8_t **msg, size_t *len);  /* a complete request is waiting */
void dirtyjtag_vault_done(void);                              /* request consumed, accept the next one */
void dirtyjtag_vault_reply(const uint8_t *msg, size_t len);   /* one message of the answer */
void dirtyjtag_vault_end(void);                               /* answer complete */

#endif
