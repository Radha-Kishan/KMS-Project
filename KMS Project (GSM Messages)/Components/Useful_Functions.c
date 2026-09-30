#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/uart.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "utils/uartstdio.h"
#include "driverlib/can.h"
#include <string.h>
#include "driverlib/interrupt.h"
#include <stdio.h>
#include "inc/hw_ints.h"

#include "Components/gsm_messages.h"
#include "main.h"
#include "Components/Useful_Functions.h"

uint32_t sysClk;

void delay_ms(uint32_t ms)
{
   SysCtlDelay((sysClk / 3 / 1000) * ms);
}

void UART7_SendString(const char *str)
{
    while (*str)
    {
        UARTCharPut(UART7_BASE, *str++);
    }
}

void send_hex_as_text(uint8_t *data, uint8_t len)
{
    char hex[4];
    uint8_t i;
    for (i = 0; i < len; i++)
    {
        sprintf(hex, "%02X", data[i]);
        UART7_SendString(hex);
    }
}

void UART7_SendBytes(uint8_t *data, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        UARTCharPut(UART7_BASE, data[i]);
}

/* Month day table (private) */
static const uint8_t days_in_month[12] =
{
    31, /* Jan */
    28, /* Feb */
    31, /* Mar */
    30, /* Apr */
    31, /* May */
    30, /* Jun */
    31, /* Jul */
    31, /* Aug */
    30, /* Sep */
    31, /* Oct */
    30, /* Nov */
    31  /* Dec */
};

/* Leap year check (private) */
static inline uint8_t is_leap_year(uint16_t year)
{
    return ((year % 4U == 0U && year % 100U != 0U) ||
            (year % 400U == 0U));
}

/* Calendar conversion (public) */
void seconds_to_calendar(uint32_t seconds,
                         uint8_t *year,
                         uint8_t *month,
                         uint8_t *day,
                         uint8_t *hour,
                         uint8_t *min,
                         uint8_t *sec)
{
    uint32_t days;
    uint16_t y;
    uint8_t m;

    /* Add GPS epoch offset */
    seconds += (GPS_EPOCH_DAY_OFFSET * SECONDS_PER_DAY);

    *sec  = seconds % 60U;
    seconds /= 60U;

    *min  = seconds % 60U;
    seconds /= 60U;

    *hour = seconds % 24U;
    days  = seconds / 24U;

    /* Compute year */
    for (y = GPS_EPOCH_YEAR; ; y++)
    {
        uint16_t year_days = is_leap_year(y) ? 366U : 365U;

        if (days < year_days)
            break;

        days -= year_days;
    }

    *year = (uint8_t)(y % 100);

    /* Compute month */
    for (m = 1; m <= 12; m++)
    {
        uint8_t dim = days_in_month[m - 1];

        if (m == 2 && is_leap_year(y))
            dim++;

        if (days < dim)
            break;

        days -= dim;
    }

    *month = m;

    /* Compute day */
    *day = (uint8_t)(days + 1U);
}

