/*
 * ML-KEM (FIPS 203) over CTAPHID vendor command 0xC3 - binary, no JSON (the messages are several kilobytes).
 *   request : op(1) | level(1: 0/1/2 = ML-KEM-512/768/1024) | data
 *     op 1 keygen : (no data)          -> response status 0 | blob | ek      (the FPGA seeds from its TRNG; blob = key wrapped under the device master)
 *     op 2 encaps : ek                 -> response status 0 | K(32) | ct
 *     op 3 decaps : blob | ct          -> response status 0 | K(32)           (implicit rejection: a bad ct gives a pseudo-random K)
 *   response: status(1) | data; status 1 = FPGA / ML-KEM unavailable, 2 = bad request length / level, 3 = FPGA refused (bad key, tampered blob), 5 = vault locked
 * The vault PIN must have been verified in this session (the host-side tools are ordinary clients of the key store).
 */
#ifndef KEM_CMD_H
#define KEM_CMD_H

#include <stdint.h>
#include <stddef.h>

void kem_handle(uint32_t channel, const uint8_t *data, size_t len);

#endif
