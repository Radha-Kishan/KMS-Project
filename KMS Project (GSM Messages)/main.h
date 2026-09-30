#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "driverlib/can.h"

#define SOF1 0XA5                                       // START OF FRAME DATA FOR GSM
#define SOF2 0XC3                                       // START OF FRAME DATA FOR GSM
#define SIM_ID 0X01
#define CRC 0x00
#define LOCO_ID 0X22

#define MESSEGE_TYPE_IDENTIFICATION_MESSEGE 0X90
#define MESSEGE_TYPE_IDENTIFICATION_ACK_MESSEGE 0X91
#define MESSEGE_TYPE_AUTH_KEY_REQ_MESSEGE 0X92
#define MESSEGE_TYPE_AUTH_KEY_MESSEGE 0X93
#define MESSEGE_TYPE_AUTH_QUERY_MESSEGE 0X94
#define MESSEGE_TYPE_AUTH_QUERY_STATUS_MESSEGE 0x95
#define MESSEGE_LENGTH_LSB_20 0X12
#define MESSEGE_LENGTH_LSB_24 0X16
#define MESSEGE_LENGTH_LSB_19 0X13
#define MESSEGE_LENGTH_MSB 0X00

#define GPS_EPOCH_YEAR 1980
#define GPS_EPOCH_DAY_OFFSET 5U   /* 6 Jan = day 5 from Jan 1 */
#define SECONDS_PER_DAY 86400UL

#define MAX_RETRY 5
#define STATE_TIMEOUT_TICKS 500000UL   /* crude busy-wait timeout, shared by all AT-wait states */

/* FLAGS */
extern volatile bool otp_received;
extern volatile bool identification_acknowledge_received;
extern volatile bool auth_key_received;
extern volatile bool auth_key_status_received;
extern volatile bool at_ok;
extern volatile bool tcp_opened;
extern volatile bool tcp_failed;

extern volatile bool mqtt_open_ok;
extern volatile bool mqtt_conn_ok;
extern volatile bool mqtt_sub_ok;

extern bool command_sent;

extern char IDENTIFICATION_ACKNOWLEDGE_MESSEGE[20];
extern uint8_t AUTHENTICATION_KEY_MESSEGE[64];
extern uint8_t AUTHENTICATION_KEY_STATUS_MESSEGE[23];

/* Decoded binary payload from the most recent +QMTRECV: URC */
extern uint8_t MQTT_RX_DATA[64];
extern uint8_t MQTT_RX_LEN;

extern uint8_t CPU_TIME_STS[8];                        // BUFFER FOR RECEIVING THE CPU TIME VIA CAN
extern uint8_t GSM_START_REQ[8];                       // BUFFER FOR RECEIVING THE GSM START MESSEGE
extern uint8_t GSM_AUTH_KEY_DATA[8];                   // BUFFER FOR TRANSMITTING AUTH KEY TO CPU VIA CAN
extern uint8_t GSM_START_ACK[8];                       // BUFFER FOR TRANSMITTING THE ACK OF THE START MESSEGE VIA CAN

/* BUFFERS */
extern char rxBuffer[256];

/* RETRY + CONTROL */
extern uint8_t retry_count;
extern bool gsm_started;
extern bool line_ready_flag;

/* TIME */
extern uint32_t seconds;

/* IDs */
extern uint8_t TCAS_ID[3];
extern uint8_t OTP[4];

extern uint8_t year, month, day, hour, min, sec;
extern uint8_t ACK_STATUS[1] ;  // success


/* ENUMS */
typedef enum {
    MODEM_POWER_ON = 0,
    MODEM_SEND_AT,
    MODEM_SET_TEXT_MODE,        // AT+CMGF=1  -> SMS in readable text, not PDU
    MODEM_SET_SMS_INDICATION,   // AT+CNMI    -> forward incoming SMS to UART automatically
    MODEM_SET_APN,
    MODEM_SET_PDP,
    MODEM_ACTIVATE_PDP,
    MODEM_MQTT_OPEN,
    MODEM_MQTT_CONNECT,
    MODEM_MQTT_SUB,
    MODEM_READY
    /* NOTE: there is no MODEM_PUBLISH_MESSEGE state anymore.
     * Every outgoing frame (identification message, auth key request,
     * auth query message, ...) opens its own AT+QMTPUB session inline,
     * inside gsm_messages.c's mqtt_publish_hex_frame(). Publishing is
     * triggered by gsm_protocol_task(), not by modem_task(). */
} modem_state_t;

extern modem_state_t modem_state;

typedef enum {                                 // ENUM FOR THE DIFFERENT STATES OF THE GSM
    GSM_START =0,
    GSM_SEND_ID,
    GSM_WAIT_OTP,
    GSM_WAIT_IDENTIFICATION_ACKNOWLEDGE,
    GSM_SEND_AUTH_REQ,
    GSM_WAIT_AUTH_KEY,
    GSM_SEND_AUTH_QUERY_MESSEGE,
    GSM_WAIT_AUTH_KEY_STATUS_MESSEGE,
    GSM_DONE
} gsm_state_t;

extern gsm_state_t gsm_state ;

/* SYSTEM CLOCK */
extern uint32_t sysClk;

/* FUNCTION */
void delay_ms(uint32_t ms);

void UART7_SendBytes(uint8_t *data, uint8_t len);
void send_hex_as_text(uint8_t *data, uint8_t len);

#endif
