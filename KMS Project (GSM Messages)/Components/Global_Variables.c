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

bool command_sent = false;
volatile bool otp_received = false;
volatile bool identification_acknowledge_received = false;
volatile bool auth_key_received = false;
volatile bool auth_key_status_received = false;
volatile bool at_ok = false;
volatile bool tcp_opened = false;
volatile bool tcp_failed = false;

volatile bool mqtt_open_ok = false;
volatile bool mqtt_conn_ok = false;
volatile bool mqtt_sub_ok  = false;

uint8_t TCAS_ID[3]={0X12,0X23,0X34};
uint8_t OTP[4];
uint8_t ACK_STATUS[1]={0X01};

uint8_t year  = 25;
uint8_t month = 2;
uint8_t day   = 9;
uint8_t hour  = 12;
uint8_t min   = 30;
uint8_t sec   = 0;

char rxBuffer[256];

uint8_t retry_count = 0;
bool gsm_started = false;
bool line_ready_flag = false;

char IDENTIFICATION_ACKNOWLEDGE_MESSEGE[20];
uint8_t AUTHENTICATION_KEY_MESSEGE[64];
uint8_t AUTHENTICATION_KEY_STATUS_MESSEGE[23];
uint32_t seconds = 0;

uint8_t MQTT_RX_DATA[64];
uint8_t MQTT_RX_LEN = 0;

uint8_t CPU_TIME_STS[8];                        // BUFFER FOR RECEIVING THE CPU TIME VIA CAN
uint8_t GSM_START_REQ[8];                       // BUFFER FOR RECEIVING THE GSM START MESSEGE
uint8_t GSM_AUTH_KEY_DATA[8];                   // BUFFER FOR TRANSMITTING AUTH KEY TO CPU VIA CAN
uint8_t GSM_START_ACK[8];                       // BUFFER FOR TRANSMITTING THE ACK OF THE START MESSEGE VIA CAN

/* STATE MACHINES */
modem_state_t modem_state = MODEM_POWER_ON;
gsm_state_t gsm_state = GSM_START;
