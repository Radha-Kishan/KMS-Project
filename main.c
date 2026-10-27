//Complete Main.c for KMS
//
// Hercules --CAN0 (any frame on 0x130)--> Tiva : start
// Tiva <--GSM / MQTT--> KMS : Query / Identification / OTP / Key Request / Keys
// Tiva --CAN0 (0x134, type 0x03)--> Hercules : key set valid now
//
// The Hercules only sends the start; everything else is done here.
// CAN (startup, heartbeat, start, CPU time, ACK) is handled in
// CAN0IntHandler() (Components/kms_can.c).

#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/uart.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/systick.h"
#include "driverlib/can.h"
#include <string.h>
#include "driverlib/interrupt.h"
#include <stdio.h>
#include "inc/hw_ints.h"

#include "Components/gsm_messages.h"
#include "main.h"
#include "Components/Useful_Functions.h"
#include "Components/kms_can.h"
#include "Components/kms_keys.h"

// Firmware checksum sent in the startup ACK (same CRC as RFID / NMS),
// from the start of flash to the end of the program (set by the linker).
extern uint8_t __FLASH_IMAGE_END;

#define FIRMWARE_CRC_START 0x00000000UL
#define FIRMWARE_CRC_END   ((uint32_t)&__FLASH_IMAGE_END - 1UL)

uint32_t calculated_firmware_checksum = 0;

uint32_t wait_ticks = 0;

volatile uint32_t g_msTicks = 0;

void SysTickIntHandler(void)
{
    g_msTicks++;
}

// ============================================================
// GSM RX LINE QUEUE
// The 0x93 message with 30 key sets is 1224 bytes = 2448 hex
// characters in one +QMTRECV line, so lines can be long.
// ============================================================
#define GSM_LINE_QUEUE_SIZE 4U      // power of 2
#define GSM_LINE_LEN        2600U

static char gsm_lines[GSM_LINE_QUEUE_SIZE][GSM_LINE_LEN];
static volatile uint8_t gsm_line_head = 0U;    // next slot to write (UART7 interrupt)
static volatile uint8_t gsm_line_tail = 0U;    // next slot to read (main loop)

static char     rx_line[GSM_LINE_LEN];         // line being received (UART7 interrupt)
static uint16_t rx_line_len = 0U;

static uint8_t  rx_bin[(GSM_LINE_LEN / 2U) + 4U];   // decoded +QMTRECV payload

// ============================================================
// KMS PROCESS STATE
// ============================================================
typedef enum
{
    KMS_IDLE = 0,
    KMS_SEND_QUERY,             // 0x94 -> wait 0x95, compare Key Set Unique ID
    KMS_WAIT_STATUS,
    KMS_WAIT_REQUEST_TIME,      // randomised start of the key request (SRS 4.7.6.8)
    KMS_SEND_ID,                // 0x90 -> wait OTP SMS + 0x91
    KMS_WAIT_OTP_AND_ACK,
    KMS_SEND_KEY_REQ,           // 0x92 (with OTP) -> wait 0x93
    KMS_WAIT_KEYS,
    KMS_SEND_CONFIRM_QUERY,     // 0x94 -> 0x95 confirms the Key Set Unique ID now stored
    KMS_WAIT_CONFIRM_STATUS,
    KMS_RETRY_WAIT              // 5 min, then kms_retry_state
} kms_state_t;

static kms_state_t kms_state = KMS_IDLE;
static kms_state_t kms_retry_state = KMS_IDLE;
static uint32_t kms_timer_ms = 0U;
static uint32_t kms_wait_ms = 0U;

static bool     kms_active = false;             // true after the first start from the Hercules
static uint32_t kms_last_query_ms = 0U;
static uint32_t kms_last_set_check_ms = 0U;
static int      kms_sent_set_index = -2;        // key set last sent to the Hercules (-2 = none)

static uint8_t  otp[4];                         // 4 alphanumeric OTP characters from the SMS
static bool     otp_received = false;

// Debug / diagnostics
volatile uint32_t kms_fail_count = 0U;          // timeouts / rejections, each followed by a retry
volatile uint8_t  kms_last_id_ack_status = 0U;  // last 0x91 status
volatile bool     kms_key_set_confirmed = false;

// ============================================================
// SMS OTP POLLING (AT+CMGL)
// ============================================================
#define SMS_RESP_BUF_SIZE          1024
#define SMS_RESP_TIMEOUT_MS        3000   // time to wait for one CMGL response to finish
#define SMS_POLL_INTERVAL_MS       1500   // gap between successive CMGL polls

typedef enum
{
    SMS_POLL_IDLE = 0,
    SMS_POLL_SEND_CMGL,
    SMS_POLL_WAIT_RESPONSE,
    SMS_POLL_PARSE_RESPONSE,
    SMS_POLL_WAIT_RETRY
} smsPollState_t;

static smsPollState_t sms_poll_state = SMS_POLL_IDLE;
static char     smsRespBuffer[SMS_RESP_BUF_SIZE];
static int      smsRespLen = 0;
static bool     sms_collecting = false;        // true while raw lines should be appended to smsRespBuffer
static bool     sms_response_complete = false; // true once an OK/ERROR line is seen while collecting
static uint32_t sms_poll_deadline_ms = 0;       // g_msTicks value at which the current wait expires

void Pwrkey(bool a)
{
    if (a)
    {
        GPIOPinWrite(GPIO_PORTD_BASE, GPIO_PIN_1, 0);
    }
    else
    {
        GPIOPinWrite(GPIO_PORTD_BASE, GPIO_PIN_1, GPIO_PIN_1);
    }
}

/* For Displaying CPU Checksum
Polynomial = 0xEDB88320
Initial    = 0xFFFFFFFF
Final XOR  = 0xFFFFFFFF
*/
uint32_t calculate_firmware_crc(void)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t address;
    uint8_t data;
    uint32_t bit;

    for (address = FIRMWARE_CRC_START; address <= FIRMWARE_CRC_END; address++)
    {
        data = *((volatile uint8_t *)address);

        crc ^= (uint32_t)data;

        for (bit = 0U; bit < 8U; bit++)
        {
            if (crc & 1U)
            {
                crc = (crc >> 1U) ^ 0xEDB88320UL;
            }
            else
            {
                crc = crc >> 1U;
            }
        }
    }

    return ~crc;
}

static bool is_alnum(char c)
{
    return ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z'));
}

// OTP from SMS text (SRS 4.7.8.8): "OTP for <S/L/T><KAVACH_ID>:<CODE> by RCIL"
// Also accepts "OTP=<CODE>" (manual test SMS). Takes the LAST OTP in the text,
// i.e. the newest SMS of an AT+CMGL listing. The code is kept as ASCII.
static bool extract_otp(const char *text, uint8_t out[4])
{
    const char *p = text;
    const char *last = 0;
    const char *code;
    uint8_t n;

    while ((p = strstr(p, "OTP")) != 0)
    {
        last = p;
        p += 3;
    }

    if (last == 0)
    {
        return false;
    }

    if (last[3] == '=')
    {
        code = last + 4;                    // "OTP=1234"
    }
    else
    {
        code = strchr(last, ':');           // "OTP for L27854:AB12 by RCIL"
        if (code == 0)
        {
            return false;
        }
        code++;
    }

    while (*code == ' ')
    {
        code++;
    }

    for (n = 0U; n < 4U; n++)
    {
        if (!is_alnum(code[n]))
        {
            return false;
        }
        out[n] = (uint8_t)code[n];
    }

    return true;
}

static void sms_otp_poll_task(void)
{
    switch (sms_poll_state)
    {
    case SMS_POLL_IDLE:
        sms_poll_state = SMS_POLL_SEND_CMGL;
        break;

    case SMS_POLL_SEND_CMGL:
        smsRespLen = 0;
        smsRespBuffer[0] = '\0';
        sms_response_complete = false;
        sms_collecting = true;   // gsm_parse_rx will now append raw lines below

        UART7_SendBytes((uint8_t*) "AT+CMGL=\"ALL\"\r\n",
                        sizeof("AT+CMGL=\"ALL\"\r\n") - 1);

        sms_poll_deadline_ms = g_msTicks + SMS_RESP_TIMEOUT_MS;
        sms_poll_state = SMS_POLL_WAIT_RESPONSE;
        break;

    case SMS_POLL_WAIT_RESPONSE:
        if (sms_response_complete)
        {
            sms_collecting = false;
            sms_poll_state = SMS_POLL_PARSE_RESPONSE;
        }
        else if ((int32_t)(g_msTicks - sms_poll_deadline_ms) >= 0)
        {
            sms_collecting = false;   // give up on this attempt, stop appending
            sms_poll_state = SMS_POLL_WAIT_RETRY;
            sms_poll_deadline_ms = g_msTicks + SMS_POLL_INTERVAL_MS;
        }
        break;

    case SMS_POLL_PARSE_RESPONSE:
        if (extract_otp(smsRespBuffer, otp))
        {
            otp_received = true;
            sms_poll_state = SMS_POLL_IDLE;   // ready for the next identification
        }
        else
        {
            sms_poll_state = SMS_POLL_WAIT_RETRY;
            sms_poll_deadline_ms = g_msTicks + SMS_POLL_INTERVAL_MS;
        }
        break;

    case SMS_POLL_WAIT_RETRY:
        if ((int32_t)(g_msTicks - sms_poll_deadline_ms) >= 0)
        {
            sms_poll_state = SMS_POLL_SEND_CMGL;
        }
        break;
    }
}

// Converts hex text to bytes, skipping anything that is not a hex digit
static uint16_t hex_text_to_bytes(const char *hex, uint8_t *out, uint16_t max_bytes)
{
    uint16_t n = 0U;
    bool high_nibble = true;
    uint8_t high = 0U;
    uint8_t val;
    char c;

    for (; *hex && (*hex != '"') && (n < max_bytes); hex++)
    {
        c = *hex;

        if (c >= '0' && c <= '9') val = (uint8_t)(c - '0');
        else if (c >= 'A' && c <= 'F') val = (uint8_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') val = (uint8_t)(c - 'a' + 10);
        else continue;

        if (high_nibble)
        {
            high = val;
            high_nibble = false;
        }
        else
        {
            out[n++] = (uint8_t)((high << 4) | val);
            high_nibble = true;
        }
    }

    return n;
}

void gsm_parse_rx(char *rx)
{
    char *recv_ptr;
    char *payload;
    uint16_t len;

    // KMS message: +QMTRECV: 0,<msgID>,"<topic>","<hex payload>"
    recv_ptr = strstr(rx, "+QMTRECV:");
    if (recv_ptr)
    {
        payload = strrchr(recv_ptr, ',');     // payload is the last field
        if (payload)
        {
            payload++;
            if (*payload == '"')
            {
                payload++;
            }
            len = hex_text_to_bytes(payload, rx_bin, sizeof(rx_bin));
            kms_parse_message(rx_bin, len);
        }
        return;
    }

    // OTP SMS delivered directly to the UART (or a test line "OTP=1234")
    if ((kms_state == KMS_WAIT_OTP_AND_ACK) && !otp_received && strstr(rx, "OTP"))
    {
        if (extract_otp(rx, otp))
        {
            otp_received = true;
            sms_collecting = false;
            sms_response_complete = false;
            sms_poll_state = SMS_POLL_IDLE;
        }
    }

    if (sms_collecting)
    {
        int line_len = (int)strlen(rx);
        if (smsRespLen + line_len < SMS_RESP_BUF_SIZE - 1)
        {
            memcpy(&smsRespBuffer[smsRespLen], rx, line_len);
            smsRespLen += line_len;
            smsRespBuffer[smsRespLen] = '\0';
        }

        if (strstr(rx, "OK") || strstr(rx, "ERROR"))
        {
            sms_response_complete = true;
        }
    }

    if (strstr(rx, "OK"))
        at_ok = true;

    if (strstr(rx, "+QMTOPEN: 0,0"))
    {
        mqtt_open_ok = true;
        tcp_opened = true;
    }
    else if (strstr(rx, "+QMTOPEN:"))
    {
        tcp_failed = true;   // any other result code = open failed
    }

    if (strstr(rx, "+QMTCONN:"))
    {
        if (strstr(rx, ",0,0"))
            mqtt_conn_ok = true;
        return;
    }

    if (strstr(rx, "+QMTSUB:"))
    {
        // "+QMTSUB: 0,1,0" -> result code 0 = success
        if (strstr(rx, ",0\r") || strstr(rx, ",0\n"))
            mqtt_sub_ok = true;
        return;
    }
}

// ============================================================
// KMS PROCESS
// ============================================================

static void kms_start_timer(uint32_t ms)
{
    kms_timer_ms = g_msTicks;
    kms_wait_ms = ms;
}

static bool kms_timer_expired(void)
{
    return ((g_msTicks - kms_timer_ms) >= kms_wait_ms);
}

// Wait KMS_RETRY_PERIOD_MS (5 min, SRS 4.7.6.8(a) / 4.7.7.6), then go to next_state
static void kms_retry(kms_state_t next_state)
{
    kms_fail_count++;
    kms_retry_state = next_state;
    kms_start_timer(KMS_RETRY_PERIOD_MS);
    kms_state = KMS_RETRY_WAIT;
}

// Sends the key set valid now to the Hercules, whenever it changes
// (new keys, validity rollover) or the Hercules asked again.
static void kms_update_hercules_keys(void)
{
    int index;

    if (!kms_active || ((g_msTicks - kms_last_set_check_ms) < 1000U))
    {
        return;
    }
    kms_last_set_check_ms = g_msTicks;

    index = kms_keys_current_index();

    if ((index >= 0) && (index != kms_sent_set_index))
    {
        kms_can_send_key_set(kms_keys_get((uint8_t)index));
        kms_sent_set_index = index;
    }
}

void kms_task(void)
{
    // ---------- start from the Hercules ----------

    if (kms_can_start_requested())    
    {
        kms_active = true;
        kms_sent_set_index = -2;            // (re)send the current set to the Hercules

        // Keys stored -> Query first; no keys -> Identification (SRS 4.7.6.6 / 4.7.7.6)
        if ((kms_state == KMS_IDLE) || (kms_state == KMS_RETRY_WAIT))
        {
            kms_state = kms_keys_available() ? KMS_SEND_QUERY : KMS_SEND_ID;
        }
    }

    kms_update_hercules_keys();

    // ---------- Authentication Query every 6 h ----------
    if ((kms_state == KMS_IDLE) && kms_active &&
        ((g_msTicks - kms_last_query_ms) >= KMS_QUERY_PERIOD_MS))
    {
        kms_state = kms_keys_available() ? KMS_SEND_QUERY : KMS_SEND_ID;
    }

    // Everything below talks to the KMS: wait for the MQTT connection
    if (modem_state != MODEM_READY)
    {
        return;
    }

    switch (kms_state)
    {
    case KMS_IDLE:
        break;

    // ---------- Query: are our keys the latest? ----------
    case KMS_SEND_QUERY:
        kms_rx.status_received = false;
        send_auth_query_messege();
        kms_last_query_ms = g_msTicks;
        kms_start_timer(KMS_RESPONSE_TIMEOUT_MS);
        kms_state = KMS_WAIT_STATUS;
        break;

    case KMS_WAIT_STATUS:
        if (kms_rx.status_received)
        {
            if ((kms_keys_count() > 0U) &&
                (memcmp(kms_rx.status_unique_id, kms_keys_unique_id(), 4U) == 0))
            {
                kms_state = KMS_IDLE;                       // keys are up to date
            }
            else
            {
#if KMS_RANDOM_REQUEST_DELAY
                // New key set in the KMS: first request at minute (ID mod 120)
                kms_start_timer((KMS_UNIT_ID % 120UL) * 60000UL);
                kms_state = KMS_WAIT_REQUEST_TIME;
#else
                kms_state = KMS_SEND_ID;
#endif
            }
        }
        else if (kms_timer_expired())
        {
            kms_retry(KMS_SEND_QUERY);
        }
        break;

    case KMS_WAIT_REQUEST_TIME:
        if (kms_timer_expired())
        {
            kms_state = KMS_SEND_ID;
        }
        break;

    // ---------- Identification -> OTP SMS + Identification ACK ----------
    case KMS_SEND_ID:
        kms_rx.id_ack_received = false;
        otp_received = false;
        sms_collecting = false;
        sms_poll_state = SMS_POLL_IDLE;

        // Delete old SMS so an old OTP can never be used
        UART7_SendBytes((uint8_t*) "AT+CMGD=1,4\r\n", sizeof("AT+CMGD=1,4\r\n") - 1);
        delay_ms(300);

        send_identification_message();
        kms_start_timer(KMS_OTP_TIMEOUT_MS);
        kms_state = KMS_WAIT_OTP_AND_ACK;
        break;

    case KMS_WAIT_OTP_AND_ACK:
        if (!otp_received)
        {
            sms_otp_poll_task();
        }

        if (kms_rx.id_ack_received && (kms_rx.id_ack_status != KMS_ACK_OTP_SENT))
        {
            // 0x02 ID not registered / 0x03 SMS delivery failed
            kms_last_id_ack_status = kms_rx.id_ack_status;
            sms_collecting = false;
            kms_retry(KMS_SEND_ID);
        }
        else if (kms_rx.id_ack_received && otp_received)
        {
            kms_last_id_ack_status = kms_rx.id_ack_status;
            sms_collecting = false;
            kms_state = KMS_SEND_KEY_REQ;
        }
        else if (kms_timer_expired())
        {
            sms_collecting = false;
            kms_retry(KMS_SEND_ID);
        }
        break;

    // ---------- Key Request (with OTP) -> Authentication Keys ----------
    case KMS_SEND_KEY_REQ:
        kms_rx.keys_received = false;
        send_auth_key_request(otp);
        kms_start_timer(KMS_RESPONSE_TIMEOUT_MS);
        kms_state = KMS_WAIT_KEYS;
        break;

    case KMS_WAIT_KEYS:
        if (kms_rx.keys_received)
        {
            kms_sent_set_index = -2;                        // send the new current set to the Hercules
            kms_state = KMS_SEND_CONFIRM_QUERY;
        }
        else if (kms_timer_expired())
        {
            kms_retry(KMS_SEND_ID);                         // a new OTP is needed
        }
        break;

    // ---------- Query again: confirm the Key Set Unique ID now stored ----------
    case KMS_SEND_CONFIRM_QUERY:
        kms_rx.status_received = false;
        send_auth_query_messege();
        kms_last_query_ms = g_msTicks;
        kms_start_timer(KMS_RESPONSE_TIMEOUT_MS);
        kms_state = KMS_WAIT_CONFIRM_STATUS;
        break;

    case KMS_WAIT_CONFIRM_STATUS:
        if (kms_rx.status_received)
        {
            kms_key_set_confirmed = (memcmp(kms_rx.status_unique_id, kms_keys_unique_id(), 4U) == 0);
            kms_state = KMS_IDLE;
        }
        else if (kms_timer_expired())
        {
            kms_key_set_confirmed = false;                  // keys are stored; next 6 h query checks again
            kms_state = KMS_IDLE;
        }
        break;

    case KMS_RETRY_WAIT:
        if (kms_timer_expired())
        {
            kms_state = kms_retry_state;
        }
        break;

    default:
        kms_state = KMS_IDLE;
        break;
    }
}

void modem_task(void)
{
    switch (modem_state)
    {
    case MODEM_POWER_ON:

        if (!command_sent)
        {
            Pwrkey(true); // THESE COMMANDS ARE USED FOR POWERING UP THE GSM MODULE
            delay_ms(2000);
            Pwrkey(false);
            delay_ms(30000);
            command_sent = true;
        }

        modem_state = MODEM_SEND_AT; // STATE CHANGED FOR SENDING THE STARTING AT COMMANDS
        command_sent = false;
        break;

    case MODEM_SEND_AT:

        if (!command_sent)
        {
            UART7_SendBytes((uint8_t*) "AT\r\n", 4);
            command_sent = true;
            wait_ticks = 0;   // reset timeout counter
        }

        if (at_ok)
        {
            at_ok = false;
            command_sent = false;
            modem_state = MODEM_SET_TEXT_MODE;
        }
        else
        {
            wait_ticks++;

            if (wait_ticks > STATE_TIMEOUT_TICKS)   // crude delay
            {
                retry_count++;

                command_sent = false; // resend AT
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_POWER_ON;
                }
            }
        }

        break;

    case MODEM_SET_TEXT_MODE:

        if (!command_sent)
        {
            retry_count = 0;
            UART7_SendBytes((uint8_t*) "AT+CMGF=1\r\n",
                            sizeof("AT+CMGF=1\r\n") - 1);   // SMS in readable text, not PDU
            command_sent = true;
            wait_ticks = 0;
        }

        if (at_ok)
        {
            at_ok = false;
            command_sent = false;
            modem_state = MODEM_SET_SMS_INDICATION;
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;
                wait_ticks = 0;
                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_SEND_AT;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_SET_SMS_INDICATION:

        if (!command_sent)
        {
            retry_count = 0;
            // Forward every new SMS straight to UART as "+CMT: ..." + body,
            // without needing to be manually read out of module memory.

            UART7_SendBytes((uint8_t*) "AT+CSCS?\r\n",
                            sizeof("AT+CSCS?\r\n") - 1);
            delay_ms(20);

            UART7_SendBytes((uint8_t*) "AT+CSGS?\r\n",
                            sizeof("AT+CSGS?\r\n") - 1);
            delay_ms(20);

            UART7_SendBytes((uint8_t*) "AT+CPMS=\"ME\",\"ME\",\"ME\"\r\n",
                            sizeof("AT+CPMS=\"ME\",\"ME\",\"ME\"\r\n") - 1);
            delay_ms(20);

            UART7_SendBytes((uint8_t*) "AT+CMGD=1,4\r\n",
                            sizeof("AT+CMGD=1,4\r\n") - 1);
            delay_ms(20);

            command_sent = true;
            wait_ticks = 0;
        }

        if (at_ok)
        {
            at_ok = false;
            command_sent = false;
            modem_state = MODEM_SET_APN;
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;
                wait_ticks = 0;
                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_SET_TEXT_MODE;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_SET_APN:

        if (!command_sent)
        {
            retry_count = 0;
            UART7_SendBytes((uint8_t*) "AT+QICSGP=1,1,\"airtelgprs.com\",\"\",\"\",0\r\n",
                            sizeof("AT+QICSGP=1,1,\"airtelgprs.com\",\"\",\"\",0\r\n") - 1);
            command_sent = true;
            wait_ticks = 0;
        }

        if (at_ok)
        {
            at_ok = false;
            command_sent = false;
            modem_state = MODEM_SET_PDP;
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_SEND_AT;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_SET_PDP:

        if (!command_sent)
        {
            retry_count = 0;
            UART7_SendBytes((uint8_t*)  "AT+CGDCONT=1,\"IP\",\"www\"\r\n",
                            sizeof("AT+CGDCONT=1,\"IP\",\"www\"\r\n") - 1); // FOR SETTING UP THE PDP CONTEXT
            command_sent = true;
            wait_ticks = 0;
        }

        if (at_ok)
        {
            at_ok = false;
            command_sent = false;
            modem_state = MODEM_ACTIVATE_PDP;
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_SET_APN;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_ACTIVATE_PDP:

        if (!command_sent)
        {
            retry_count = 0;
            UART7_SendBytes((uint8_t*) "AT+QIACT=1\r\n",
                            sizeof("AT+QIACT=1\r\n") - 1); // COMMAND FOR ACTIVATING UP THE PDP
            command_sent = true;
            wait_ticks = 0;
        }

        if (at_ok)
        {
            at_ok = false;
            command_sent = false;
            modem_state = MODEM_MQTT_OPEN;
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_SET_PDP;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_MQTT_OPEN:

        if (!command_sent)
        {
            retry_count = 0;
            tcp_failed = false;

            // Clear any stale MQTT session left over from a previous run
            UART7_SendBytes((uint8_t*) "AT+QMTCLOSE=0\r\n",
                            sizeof("AT+QMTCLOSE=0\r\n") - 1);
            delay_ms(1000);

            UART7_SendBytes(
                    (uint8_t*) "AT+QMTOPEN=0,\"broker.hivemq.com\",1883\r\n",
                    sizeof("AT+QMTOPEN=0,\"broker.hivemq.com\",1883\r\n") - 1);
            command_sent = true;
            wait_ticks = 0;
        }

        if (mqtt_open_ok)   // set when "+QMTOPEN: 0,0" is seen
        {
            mqtt_open_ok = false;
            command_sent = false;
            modem_state = MODEM_MQTT_CONNECT;
        }
        else if (tcp_failed)
        {
            tcp_failed = false;
            retry_count++;
            command_sent = false;   // retry (will QMTCLOSE + QMTOPEN again)
            wait_ticks = 0;

            if (retry_count >= MAX_RETRY)
            {
                modem_state = MODEM_ACTIVATE_PDP; // fall back and re-activate PDP
                command_sent = false;
            }
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;   // no response at all -> retry
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_ACTIVATE_PDP;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_MQTT_CONNECT:

        if (!command_sent)
        {
            retry_count = 0;
            UART7_SendBytes(
                (uint8_t*)"AT+QMTCONN=0,\"EC300CLIENT\"\r\n",
                sizeof("AT+QMTCONN=0,\"EC300CLIENT\"\r\n") - 1);
            command_sent = true;
            wait_ticks = 0;
        }

        if (mqtt_conn_ok)
        {
            mqtt_conn_ok = false;
            command_sent = false;
            modem_state = MODEM_MQTT_SUB;
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;   // retry connect
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    modem_state = MODEM_MQTT_OPEN;  // re-open and retry from there
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_MQTT_SUB:

        if (!command_sent)
        {
            retry_count = 0;
            UART7_SendBytes((uint8_t*) "AT+QMTSUB=0,1,\"rk/gsm/rx\",0\r\n",
                            sizeof("AT+QMTSUB=0,1,\"rk/gsm/rx\",0\r\n") - 1);
            command_sent = true;
            wait_ticks = 0;
        }

        if (mqtt_sub_ok)
        {
            mqtt_sub_ok = false;
            command_sent = false;
            modem_state = MODEM_READY;   // publishing is now handled per-message by gsm_protocol_task
        }
        else
        {
            wait_ticks++;
            if (wait_ticks > STATE_TIMEOUT_TICKS)
            {
                retry_count++;
                command_sent = false;   // retry subscribe
                wait_ticks = 0;

                if (retry_count >= MAX_RETRY)
                {
                    // give up subscribing, still proceed to READY
                    modem_state = MODEM_READY;
                    command_sent = false;
                }
            }
        }
        break;

    case MODEM_READY:                 // NOW MODEM IS READY TO TRANSMIT THE DATA
        // Do nothing
        break;

    default:
        break;
    }
}


void UART7IntHandler(void)
{
    uint32_t status = UARTIntStatus(UART7_BASE, true);
    UARTIntClear(UART7_BASE, status);

    while(UARTCharsAvail(UART7_BASE))
    {
        char c = UARTCharGetNonBlocking(UART7_BASE);

        if (rx_line_len < (GSM_LINE_LEN - 1U))
            rx_line[rx_line_len++] = c;

        if ((c == '\n') || (rx_line_len >= (GSM_LINE_LEN - 1U)))
        {
            rx_line[rx_line_len] = '\0';

            // Queue the line; if the queue is full the line is dropped
            if ((uint8_t)(gsm_line_head - gsm_line_tail) < GSM_LINE_QUEUE_SIZE)
            {
                memcpy(gsm_lines[gsm_line_head % GSM_LINE_QUEUE_SIZE], rx_line, rx_line_len + 1U);
                gsm_line_head++;
            }
            rx_line_len = 0U;
        }
    }
}

int main(void)
{
    sysClk = SysCtlClockFreqSet(
            SYSCTL_USE_PLL | SYSCTL_OSC_MAIN |
            SYSCTL_XTAL_25MHZ | SYSCTL_CFG_VCO_480,
            120000000);

    // 1 ms SysTick, used for all KMS / SMS timeouts
    SysTickPeriodSet(sysClk / 1000);
    SysTickIntEnable();
    SysTickEnable();

    calculated_firmware_checksum = calculate_firmware_crc();

    // Key store in RAM, empty until the KMS sends keys
    kms_keys_init();

    // CAN first, so startup / heartbeat are answered while the modem powers up
    // (CAN0 uses PA0/PA1, so the UART0 debug console is no longer available)
    kms_can_init(calculated_firmware_checksum);

    // Enable UART7 and GPIO Port C
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART7);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART7));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOC));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOD));

    // Configure UART7 TX/RX pins (PC4/PC5)
    GPIOPinConfigure(GPIO_PC4_U7RX);
    GPIOPinConfigure(GPIO_PC5_U7TX);
    GPIOPinTypeUART(GPIO_PORTC_BASE, GPIO_PIN_4 | GPIO_PIN_5);

    // Configure UART7
    UARTConfigSetExpClk(UART7_BASE, sysClk, 115200,
                        UART_CONFIG_WLEN_8 |
                        UART_CONFIG_STOP_ONE |
                        UART_CONFIG_PAR_NONE);

    GPIOPinTypeGPIOOutput(GPIO_PORTD_BASE, GPIO_PIN_1);
    UARTIntEnable(UART7_BASE, UART_INT_RX | UART_INT_RT);
    IntEnable(INT_UART7);
    IntMasterEnable();

    Pwrkey(false);

    while (1)
    {
        while (gsm_line_tail != gsm_line_head) // PARSE EVERY LINE RECEIVED FROM THE GSM MODULE
        {
            gsm_parse_rx(gsm_lines[gsm_line_tail % GSM_LINE_QUEUE_SIZE]);
            gsm_line_tail++;
        }

        modem_task(); // MODEM STATE MACHINE FOR SENDING THE AT COMMANDS FOR SETTING UP CONNECTION

        kms_task();   // KMS PROCESS, STARTED BY THE HERCULES

        kms_can_task(); // RESENDS THE KEY SET TO THE HERCULES UNTIL IT IS ACKED
    }
}
