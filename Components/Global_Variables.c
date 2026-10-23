#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/uart.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/can.h"
#include <string.h>
#include "driverlib/interrupt.h"
#include <stdio.h>
#include "inc/hw_ints.h"

#include "Components/gsm_messages.h"
#include "main.h"
#include "Components/Useful_Functions.h"

bool command_sent = false;
volatile bool at_ok = false;
volatile bool tcp_opened = false;
volatile bool tcp_failed = false;

volatile bool mqtt_open_ok = false;
volatile bool mqtt_conn_ok = false;
volatile bool mqtt_sub_ok  = false;

uint8_t retry_count = 0;

/* STATE MACHINES */
modem_state_t modem_state = MODEM_POWER_ON;
