#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Performs the full BLE handshake:
// 1. Loads encRand from NVS, or runs V0 pairing to extract it and saves it.
// 2. Runs CommCmd login (action=64) + time-sync (action=104).
// sn: the inverter's 12-char serial (discovered over BLE), used for V0 key/IV
// derivation and the CommCmd dtu_sn.
// tid: monotonic transaction ID (incremented by this function).
// Returns true on success. On success, enc_rand_out holds the 16-byte encRand.
bool handshake_run(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]);

// Clears the stored encRand from NVS (call if V1 decryption fails repeatedly).
void handshake_clear_nvs(void);

#ifdef __cplusplus
}
#endif
