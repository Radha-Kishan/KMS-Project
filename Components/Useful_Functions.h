#ifndef USEFUL_FUNCTION_H
#define USEFUL_FUNCTION_H

#include <stdint.h>
#include <stddef.h>

/* CRC-32: poly 0x04C11DB7 (reflected 0xEDB88320), init 0, no final XOR (SRS 4.7.9) */
uint32_t calculate_crc32(const uint8_t *data, size_t length);

/* Convert GPS seconds into calendar date/time */
void seconds_to_calendar(uint32_t seconds,
                         uint8_t *year,
                         uint8_t *month,
                         uint8_t *day,
                         uint8_t *hour,
                         uint8_t *min,
                         uint8_t *sec);

#endif
