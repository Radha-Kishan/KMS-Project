#ifndef KMS_KEYS_H
#define KMS_KEYS_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

// One Authentication Key set, exactly as in the 0x93 message (40 bytes):
// validity start (HH DD MM YY), validity end (HH DD MM YY), key 1, key 2.
// The Hercules (kms.c, packet type 0x03) expects the same 40-byte layout.
typedef struct
{
    uint8_t start[4];
    uint8_t end[4];
    uint8_t key1[16];
    uint8_t key2[16];
} kms_key_set_t;

#define KMS_KEY_SET_BYTES   40U

// Current date / time in IST, as used in the KMS messages
typedef struct
{
    uint8_t day, month, year;       // year = 00..99
    uint8_t hour, min, sec;
} kms_datetime_t;

void kms_keys_init(void);

bool kms_keys_save(const uint8_t unique_id[4], const kms_key_set_t *sets, uint8_t count);

uint8_t kms_keys_count(void);
const uint8_t *kms_keys_unique_id(void);
const kms_key_set_t *kms_keys_get(uint8_t index);

// Index of the set valid right now, -1 if none or the time is not known
int kms_keys_current_index(void);

// true if keys are stored and (the time is unknown or a set is valid now)
bool kms_keys_available(void);

// Current IST date/time from the Hercules CPU time. false if no valid time yet.
bool kms_time_now(kms_datetime_t *t);

#endif
