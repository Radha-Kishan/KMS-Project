#ifndef USEFUL_FUNCTION_H
#define USEFUL_FUNCTION_H

#include <stdint.h>

/* Convert GPS seconds into calendar date/time */
void seconds_to_calendar(uint32_t seconds,
                         uint8_t *year,
                         uint8_t *month,
                         uint8_t *day,
                         uint8_t *hour,
                         uint8_t *min,
                         uint8_t *sec);

#endif
