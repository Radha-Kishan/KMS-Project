#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "driverlib/can.h"

// ============================================================
// KAVACH UNIT IDENTITY (sent in every message to the KMS)
// ============================================================
#define KMS_UNIT_TYPE       0x22U       // 0x11 Stationary, 0x22 Onboard, 0x33 TSRMS
#define KMS_UNIT_ID         27854UL     // KAVACH unit ID (3 bytes) - same as kavach_subsystem_id on the Hercules
#define SIM_ID              0x01U       // 0x01 Primary, 0x02 Secondary

// ============================================================
// KMS PROTOCOL (SRS Amdt-3, 4.7.8)
// ============================================================
#define SOF1 0xA5U                                      // START OF FRAME DATA FOR GSM
#define SOF2 0xC3U                                      // START OF FRAME DATA FOR GSM

#define MESSEGE_TYPE_IDENTIFICATION_MESSEGE     0x90U
#define MESSEGE_TYPE_IDENTIFICATION_ACK_MESSEGE 0x91U
#define MESSEGE_TYPE_AUTH_KEY_REQ_MESSEGE       0x92U
#define MESSEGE_TYPE_AUTH_KEY_MESSEGE           0x93U
#define MESSEGE_TYPE_AUTH_QUERY_MESSEGE         0x94U
#define MESSEGE_TYPE_AUTH_QUERY_STATUS_MESSEGE  0x95U

// Identification Acknowledge status (0x91)
#define KMS_ACK_OTP_SENT            0x01U
#define KMS_ACK_ID_NOT_REGISTERED   0x02U
#define KMS_ACK_DELIVERY_FAILED     0x03U

#define KMS_MAX_KEY_SETS            30U         // KMS sends 0x1E sets

// MQTT topics (test setup - the SRS transport is UDP to the KMS URL/IP)
#define KMS_TX_TOPIC   "rk/gsm/messages"
#define KMS_RX_TOPIC   "rk/gsm/rx"

// 1 = drop KMS replies with a wrong CRC or a different unit type / ID.
// 0 = accept them (useful while testing with hand-made MQTT messages).
#define KMS_STRICT_RX_CHECK         0

// ============================================================
// KMS TIMING (SRS 4.7.6 / 4.7.7)
// ============================================================
#define KMS_QUERY_PERIOD_MS         (6UL * 60UL * 60UL * 1000UL)    // Authentication Query every 6 h
#define KMS_RETRY_PERIOD_MS         (5UL * 60UL * 1000UL)           // retry every 5 min
#define KMS_RESPONSE_TIMEOUT_MS     60000UL                         // wait for 0x93 / 0x95
#define KMS_OTP_TIMEOUT_MS          120000UL                        // wait for OTP SMS + 0x91

// 1 = new keys are requested at minute (unit ID mod 120), as SRS 4.7.6.8.
// 0 = request immediately (useful while testing).
#define KMS_RANDOM_REQUEST_DELAY    1

// ============================================================
// TIME (CPU time from the Hercules is GPS seconds)
// ============================================================
#define GPS_EPOCH_YEAR 1980
#define GPS_EPOCH_DAY_OFFSET 5U   /* 6 Jan = day 5 from Jan 1 */
#define SECONDS_PER_DAY 86400UL
#define GPS_UTC_LEAP_SECONDS        18UL
#define IST_OFFSET_SECONDS          19800UL     // +05:30

#define MAX_RETRY 5
#define STATE_TIMEOUT_TICKS 500000UL   /* crude busy-wait timeout, shared by all AT-wait states */

/* FLAGS */
extern volatile bool at_ok;
extern volatile bool tcp_opened;
extern volatile bool tcp_failed;

extern volatile bool mqtt_open_ok;
extern volatile bool mqtt_conn_ok;
extern volatile bool mqtt_sub_ok;

extern bool command_sent;

/* RETRY + CONTROL */
extern uint8_t retry_count;

/* 1 ms tick */
extern volatile uint32_t g_msTicks;

/* ENUMS */
typedef enum {
    MODEM_POWER_ON = 0,
    MODEM_SEND_AT,
    MODEM_SET_TEXT_MODE,        // AT+CMGF=1  -> SMS in readable text, not PDU
    MODEM_SET_SMS_INDICATION,   // SMS storage settings + delete old SMS
    MODEM_SET_APN,
    MODEM_SET_PDP,
    MODEM_ACTIVATE_PDP,
    MODEM_MQTT_OPEN,
    MODEM_MQTT_CONNECT,
    MODEM_MQTT_SUB,
    MODEM_READY
    /* Every outgoing KMS message opens its own AT+QMTPUB session inline,
     * inside gsm_messages.c's mqtt_publish_hex_frame(). */
} modem_state_t;

extern modem_state_t modem_state;

/* SYSTEM CLOCK */
extern uint32_t sysClk;

/* FUNCTION */
void delay_ms(uint32_t ms);

void UART7_SendBytes(uint8_t *data, uint8_t len);
void send_hex_as_text(uint8_t *data, uint8_t len);

#endif
