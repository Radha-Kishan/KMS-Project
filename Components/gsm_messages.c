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
#include "Components/kms_keys.h"

// ============================================================
// COMMON KMS FRAME (SRS 4.7.8.2 - 4.7.8.7)
//
// Byte 0-1   : SOF 0xA5 0xC3
// Byte 2     : Message Type
// Byte 3-4   : Message Length = bytes from Date to CRC, inclusive (LSB first)
// Byte 5-7   : Date DD MM YY (IST)
// Byte 8-10  : Time HH MM SS (IST)
// Byte 11    : Type of KAVACH unit
// Byte 12-14 : KAVACH Unit ID
// ...        : message fields
// last 4     : 32-bit CCITT CRC from Message Type up to the last field (SOF excluded)
// ============================================================
#define KMS_HDR_LEN         15U     // up to and including the unit ID
#define KMS_CRC_LEN         4U
#define KMS_DATE_OFFSET     5U      // Message Length counts from here

kms_rx_result_t kms_rx;

// ---------- 0x93 key sets, parsed straight out of the message ----------
static kms_key_set_t rx_sets[KMS_MAX_KEY_SETS];

static void mqtt_publish_hex_frame(const char *topic, uint8_t *data, uint8_t len)
{
    char cmd[64];
    int cmd_len = snprintf(cmd, sizeof(cmd), "AT+QMTPUB=0,0,0,0,\"%s\"\r\n", topic);

    UART7_SendBytes((uint8_t*) cmd, (uint8_t) cmd_len);
    delay_ms(1000);              // wait for the module's '>' data prompt

    send_hex_as_text(data, len);
    delay_ms(200);
    UARTCharPut(UART7_BASE, 26); // CTRL+Z to send
    delay_ms(500);
}

// Header up to the unit ID. Date/time from the Hercules CPU time (0xFF = unknown).
static void kms_build_header(uint8_t *buf, uint8_t msg_type, uint16_t total_len)
{
    kms_datetime_t now;
    uint16_t msg_len = (uint16_t)(total_len - KMS_DATE_OFFSET);

    if (!kms_time_now(&now))
    {
        memset(&now, 0xFF, sizeof(now));
    }

    buf[0]  = SOF1;
    buf[1]  = SOF2;
    buf[2]  = msg_type;
    buf[3]  = (uint8_t)(msg_len & 0xFFU);
    buf[4]  = (uint8_t)(msg_len >> 8);
    buf[5]  = now.day;
    buf[6]  = now.month;
    buf[7]  = now.year;
    buf[8]  = now.hour;
    buf[9]  = now.min;
    buf[10] = now.sec;
    buf[11] = KMS_UNIT_TYPE;
    buf[12] = (uint8_t)(KMS_UNIT_ID >> 16);
    buf[13] = (uint8_t)(KMS_UNIT_ID >> 8);
    buf[14] = (uint8_t)(KMS_UNIT_ID);
}

// CRC over Message Type .. last field, appended MSB first
static void kms_append_crc(uint8_t *buf, uint16_t total_len)
{
    uint16_t crc_pos = (uint16_t)(total_len - KMS_CRC_LEN);
    uint32_t crc = calculate_crc32(&buf[2], crc_pos - 2U);

    buf[crc_pos]      = (uint8_t)(crc >> 24);
    buf[crc_pos + 1U] = (uint8_t)(crc >> 16);
    buf[crc_pos + 2U] = (uint8_t)(crc >> 8);
    buf[crc_pos + 3U] = (uint8_t)(crc);
}

// 0x90 Identification Message, 20 bytes (Length = 15)
void send_identification_message(void)
{
    uint8_t msg[20];

    kms_build_header(msg, MESSEGE_TYPE_IDENTIFICATION_MESSEGE, sizeof(msg));
    msg[15] = SIM_ID;
    kms_append_crc(msg, sizeof(msg));

    mqtt_publish_hex_frame(KMS_TX_TOPIC, msg, sizeof(msg));
}

// 0x92 Authentication Key Request, 24 bytes (Length = 19)
void send_auth_key_request(const uint8_t otp[4])
{
    uint8_t msg[24];

    kms_build_header(msg, MESSEGE_TYPE_AUTH_KEY_REQ_MESSEGE, sizeof(msg));
    msg[15] = SIM_ID;
    memcpy(&msg[16], otp, 4U);          // OTP as received in the SMS (alphanumeric)
    kms_append_crc(msg, sizeof(msg));

    mqtt_publish_hex_frame(KMS_TX_TOPIC, msg, sizeof(msg));
}

// 0x94 Authentication Query, 19 bytes (Length = 14)
void send_auth_query_messege(void)
{
    uint8_t msg[19];

    kms_build_header(msg, MESSEGE_TYPE_AUTH_QUERY_MESSEGE, sizeof(msg));
    kms_append_crc(msg, sizeof(msg));

    mqtt_publish_hex_frame(KMS_TX_TOPIC, msg, sizeof(msg));
}

// ============================================================
// RECEIVE
// ============================================================

static bool kms_message_valid(const uint8_t *msg, uint16_t len, uint16_t min_len)
{
    if ((len < min_len) || (msg[0] != SOF1) || (msg[1] != SOF2))
    {
        return false;
    }

#if KMS_STRICT_RX_CHECK
    {
        uint32_t crc = calculate_crc32(&msg[2], len - KMS_CRC_LEN - 2U);
        uint32_t rx_crc = ((uint32_t)msg[len - 4U] << 24) | ((uint32_t)msg[len - 3U] << 16) |
                          ((uint32_t)msg[len - 2U] << 8)  |  (uint32_t)msg[len - 1U];

        if (crc != rx_crc)
        {
            return false;
        }

        if ((msg[11] != KMS_UNIT_TYPE) ||
            (msg[12] != (uint8_t)(KMS_UNIT_ID >> 16)) ||
            (msg[13] != (uint8_t)(KMS_UNIT_ID >> 8)) ||
            (msg[14] != (uint8_t)(KMS_UNIT_ID)))
        {
            return false;
        }
    }
#endif

    return true;
}

// 0x93: Key Set Unique ID (4) at 15, number of sets n (1) at 19,
// then n x [start (4), end (4), key 1 (16), key 2 (16)] from byte 20
static void kms_handle_auth_keys(const uint8_t *msg, uint16_t len)
{
    uint8_t count = msg[19];
    uint8_t i;

    if (count > KMS_MAX_KEY_SETS)
    {
        count = KMS_MAX_KEY_SETS;
    }

    // All announced sets must be in the message
    if ((uint16_t)(20U + (uint16_t)count * KMS_KEY_SET_BYTES + KMS_CRC_LEN) > len)
    {
        return;
    }

    for (i = 0U; i < count; i++)
    {
        memcpy(&rx_sets[i], &msg[20U + (uint16_t)i * KMS_KEY_SET_BYTES], KMS_KEY_SET_BYTES);
    }

    kms_keys_save(&msg[15], rx_sets, count);
    kms_rx.keys_received = true;
}

void kms_parse_message(const uint8_t *msg, uint16_t len)
{
    if (len < 3U)
    {
        return;
    }

    switch (msg[2])
    {
    case MESSEGE_TYPE_IDENTIFICATION_ACK_MESSEGE:       // 20 bytes
        if (kms_message_valid(msg, len, 20U))
        {
            kms_rx.id_ack_status = msg[15];
            kms_rx.id_ack_received = true;
        }
        break;

    case MESSEGE_TYPE_AUTH_KEY_MESSEGE:                 // 20 + 40n + 4 bytes
        if (kms_message_valid(msg, len, 24U))
        {
            kms_handle_auth_keys(msg, len);
        }
        break;

    case MESSEGE_TYPE_AUTH_QUERY_STATUS_MESSEGE:        // 23 bytes
        if (kms_message_valid(msg, len, 23U))
        {
            memcpy(kms_rx.status_unique_id, &msg[15], 4U);
            kms_rx.status_received = true;
        }
        break;

    default:
        break;
    }
}
