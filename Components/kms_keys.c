#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "main.h"
#include "Components/kms_keys.h"
#include "Components/kms_can.h"
#include "Components/Useful_Functions.h"

// ============================================================
// KEY STORE IN RAM (lost on reset - the KMS process gets new keys)
// ============================================================
typedef struct
{
    uint8_t  unique_id[4];                      // Key Set Unique ID from the KMS
    uint32_t count;                             // number of valid sets
    kms_key_set_t sets[KMS_MAX_KEY_SETS];
} kms_key_store_t;

static kms_key_store_t store;

void kms_keys_init(void)
{
    memset(&store, 0, sizeof(store));
}

void kms_keys_save(const uint8_t unique_id[4], const kms_key_set_t *sets, uint8_t count)
{
    if (count > KMS_MAX_KEY_SETS)
    {
        count = KMS_MAX_KEY_SETS;
    }

    memset(&store, 0, sizeof(store));
    memcpy(store.unique_id, unique_id, 4U);
    store.count = count;
    memcpy(store.sets, sets, (uint32_t)count * sizeof(kms_key_set_t));
}

uint8_t kms_keys_count(void)
{
    return (uint8_t)store.count;
}

const uint8_t *kms_keys_unique_id(void)
{
    return store.unique_id;
}

const kms_key_set_t *kms_keys_get(uint8_t index)
{
    return (index < store.count) ? &store.sets[index] : 0;
}

// ============================================================
// TIME
// ============================================================

bool kms_time_now(kms_datetime_t *t)
{
    uint32_t gps_sec;

    if (!kms_can_get_cpu_time(&gps_sec))
    {
        return false;
    }

    // GPS time -> UTC -> IST, still counted from the GPS epoch
    seconds_to_calendar(gps_sec - GPS_UTC_LEAP_SECONDS + IST_OFFSET_SECONDS,
                        &t->year, &t->month, &t->day,
                        &t->hour, &t->min, &t->sec);
    return true;
}

// HH DD MM YY -> one number that increases with time
static uint32_t validity_value(const uint8_t hh_dd_mm_yy[4])
{
    return ((((uint32_t)hh_dd_mm_yy[3] * 13U + hh_dd_mm_yy[2]) * 32U + hh_dd_mm_yy[1]) * 24U) + hh_dd_mm_yy[0];
}

int kms_keys_current_index(void)
{
    kms_datetime_t now;
    uint8_t now_bytes[4];
    uint32_t now_value;
    uint32_t i;

    if (!kms_time_now(&now))
    {
        return (store.count > 0U) ? 0 : -1;     // TEST: no GPS time -> use set 0      ACTUAL: return -1;
    }

    now_bytes[0] = now.hour;
    now_bytes[1] = now.day;
    now_bytes[2] = now.month;
    now_bytes[3] = now.year;
    now_value = validity_value(now_bytes);

    for (i = 0U; i < store.count; i++)
    {
        if ((validity_value(store.sets[i].start) <= now_value) &&
            (now_value < validity_value(store.sets[i].end)))
        {
            return (int)i;
        }
    }

    return (store.count > 0U) ? 0 : -1;         // TEST: no set valid now -> use set 0  ACTUAL: return -1;
}

bool kms_keys_available(void)
{
    kms_datetime_t now;

    if (store.count == 0U)
    {
        return false;
    }

    // Without a time reference the stored sets cannot be checked: assume usable
    if (!kms_time_now(&now))
    {
        return true;
    }

    return (kms_keys_current_index() >= 0);
}
