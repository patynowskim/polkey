/* Fingerprint sensor (Synochip R30x / Adafruit optical module) behind the FPGA UART bridge (fpga_fp_xfer) */
#ifndef FP_H
#define FP_H

#include <stdbool.h>
#include <stdint.h>

enum { FP_NOFINGER = 0, FP_MATCH = 1, FP_NOMATCH = 2, FP_ERR = 3 };

bool fp_present(void);                       /* sensor answered the password check (probed again every few seconds while absent) */
int  fp_enrolled(void);                      /* number of stored templates, -1 if unknown */
int  fp_poll(uint16_t *score);               /* one non-blocking attempt: image -> features -> search */
bool fp_enroll(uint16_t id, uint32_t timeout_ms);   /* two scans of the same finger; shows the prompts on the display */
bool fp_empty(void);

#define FP_MIN_SCORE 50

#endif
