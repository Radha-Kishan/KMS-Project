//Complete Main.c for KMS

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
#include "utils/uartstdio.h"
#include "driverlib/can.h"
#include <string.h>
#include "driverlib/interrupt.h"
#include <stdio.h>
#include "inc/hw_ints.h"

#include "Components/gsm_messages.h"
#include "main.h"
#include "Components/Useful_Functions.h"

tCANMsgObject rxCPUTime;
tCANMsgObject rxStartCmd;

// Buffer to store received characters
char current_buffer[256];

uint32_t wait_ticks = 0;
char MQTT_RX_TEXT[200];

// 4TH BYTE OF GSM_START_REQ DECIDES 1- START 2-STOP
// FIRST 3 BYTES OF CPU_TIME_SYS GIVES THE CPU TIME
uint8_t TIME_IN_DEC[3];

volatile uint32_t g_msTicks = 0;

void SysTickIntHandler(void)
{
    g_msTicks++;
}

#define SMS_RESP_BUF_SIZE          512
#define SMS_LAST_BUF_SIZE          200

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
static char     lastSMS[SMS_LAST_BUF_SIZE];
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

void store_otp(char *rxbuffer, uint8_t *otp)
{
    int j = 0;
    int i;
    for (i = 0; rxbuffer[i] != '\0'; i++)
    {
        if (rxbuffer[i] >= '0' && rxbuffer[i] <= '9')
        {
            otp[j++] = rxbuffer[i] - '0';  // ASCII → number
            if (j == 4)
                break;
        }
    }
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
        UARTprintf("\r\n[OTP-POLL] sent AT+CMGL=\"ALL\"\r\n");

        sms_poll_deadline_ms = g_msTicks + SMS_RESP_TIMEOUT_MS;
        sms_poll_state = SMS_POLL_WAIT_RESPONSE;
        break;

    case SMS_POLL_WAIT_RESPONSE:
        if (sms_response_complete)
        {
            sms_collecting = false;
            UARTprintf("[OTP-POLL] response complete (%d bytes)\r\n", smsRespLen);
            sms_poll_state = SMS_POLL_PARSE_RESPONSE;
        }
        else if ((int32_t)(g_msTicks - sms_poll_deadline_ms) >= 0)
        {
            sms_collecting = false;   // give up on this attempt, stop appending
            UARTprintf("[OTP-POLL] timed out waiting for CMGL response (%d bytes collected) — retrying\r\n", smsRespLen);
            sms_poll_state = SMS_POLL_WAIT_RETRY;
            sms_poll_deadline_ms = g_msTicks + SMS_POLL_INTERVAL_MS;
        }
        break;

    case SMS_POLL_PARSE_RESPONSE:
        UARTprintf("[OTP-POLL] last SMS body: \"%s\"\r\n", lastSMS);

        if (strstr(lastSMS, "OTP") != NULL)
        {
            store_otp(lastSMS, OTP);   // OTP[] as already used/declared elsewhere in your project
            otp_received = true;
            UARTprintf("[OTP-POLL] OTP found -> stored: %d%d%d%d\r\n", OTP[0], OTP[1], OTP[2], OTP[3]);

            // Delete all read messages now so a old OTP entry can never be re-matched on a future GSM_WAIT_OTP cycle.
            UART7_SendBytes((uint8_t*) "AT+CMGD=1,4\r\n", sizeof("AT+CMGD=1,4\r\n") - 1);

            sms_poll_state = SMS_POLL_IDLE;   // reset so it's ready if GSM_WAIT_OTP is ever re-entered
        }
        else
        {
            UARTprintf("[OTP-POLL] no \"OTP\" text found, retrying\r\n");
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

static void hex_to_bytes(const char *hexstr, uint8_t *out, int max_bytes, int *out_len)
{
    int n = 0;
    uint8_t nibble_buf[2];
    int nibble_count = 0;

    for (; *hexstr && n < max_bytes; hexstr++)
    {
        char c = *hexstr;
        uint8_t val;

        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else continue;   // skip quotes, \r, \n, spaces, etc.

        nibble_buf[nibble_count++] = val;
        if (nibble_count == 2)
        {
            out[n++] = (uint8_t)((nibble_buf[0] << 4) | nibble_buf[1]);
            nibble_count = 0;
        }
    }
    *out_len = n;
}

void gsm_parse_rx(char *rx)
{
    if (!otp_received)
    {
        char *otp_ptr = strstr(rx, "OTP=");
        if (otp_ptr != NULL)
        {
            store_otp(otp_ptr + 4, OTP);   // parse the digits right after "OTP="
            otp_received = true;
            UARTprintf("\r\n[OTP] fast-path match on line: \"%s\" -> stored: %d%d%d%d\r\n",
                       rx, OTP[0], OTP[1], OTP[2], OTP[3]);

            // No need to keep collecting/waiting on the in-flight CMGL poll
            sms_collecting = false;
            sms_response_complete = false;
            sms_poll_state = SMS_POLL_IDLE;
        }
    }

    if (sms_collecting)
    {
        int len = (int)strlen(rx);
        if (smsRespLen + len < SMS_RESP_BUF_SIZE - 1)
        {
            memcpy(&smsRespBuffer[smsRespLen], rx, len);
            smsRespLen += len;
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

    // Binary frames (identification ack, auth key, auth query) arrive here
    char *recv_ptr = strstr(rx, "+QMTRECV:");
       if (recv_ptr)
       {
           char *last_comma = strrchr(recv_ptr, ',');
           if (last_comma)
           {
               const char *p = last_comma + 1;
               if (*p == '"') p++;

               int n = 0;
               while (*p && *p != '"' && *p != '\r' && *p != '\n' &&
                      n < (int)sizeof(MQTT_RX_TEXT) - 1)
               {
                   MQTT_RX_TEXT[n++] = *p++;
               }
               MQTT_RX_TEXT[n] = '\0';

               int decoded_len = 0;
               hex_to_bytes(MQTT_RX_TEXT, MQTT_RX_DATA, sizeof(MQTT_RX_DATA), &decoded_len);
               MQTT_RX_LEN = (uint8_t)decoded_len;

               if (decoded_len >= 3 && MQTT_RX_DATA[0] == SOF1 && MQTT_RX_DATA[1] == SOF2)
               {
                   if (MQTT_RX_DATA[2] == MESSEGE_TYPE_IDENTIFICATION_ACK_MESSEGE)
                   {
                       store_identification_acknowledge();
                       identification_acknowledge_received = true;
                       UARTprintf("\r\n[ID_ACK] received and stored\r\n");
                   }
                   else if (MQTT_RX_DATA[2] == MESSEGE_TYPE_AUTH_KEY_MESSEGE)
                       auth_key_received = true;
                   else if (MQTT_RX_DATA[2] == MESSEGE_TYPE_AUTH_QUERY_STATUS_MESSEGE)
                       auth_key_status_received = true;
               }
           }

           return;
       }
}

void gsm_protocol_task(void)
{
    switch (gsm_state)
    // USING THE GSM STATE MACHINE
    {
    case GSM_START:                                          // INITIAL STATE OF THE GSM

        gsm_state = GSM_SEND_ID;
        break;

    case GSM_SEND_ID:

        send_identification_message();                        // builds IM_tcas_to_kms[], opens its own QMTPUB session, and publishes it
        sms_poll_state = SMS_POLL_IDLE;                       // arm the SMS poller fresh for this OTP wait
        otp_received = false;                                 // clear any stale latch before waiting again
        identification_acknowledge_received = false;
        gsm_state = GSM_WAIT_OTP;
        break;

    case GSM_WAIT_OTP:

        if (!otp_received)
            sms_otp_poll_task();   // non-blocking: AT+CMGL -> collect -> extract -> store_otp
                                    // (the fast-path "OTP=" check in gsm_parse_rx may beat this to it)

        if (otp_received && identification_acknowledge_received)
        {
            otp_received = false;
            identification_acknowledge_received = false;
            // store_identification_acknowledge() already ran the instant the
            // ID_ACK arrived (see gsm_parse_rx) — no need to call it again here
            gsm_state = GSM_SEND_AUTH_REQ;
            UARTprintf("\r\n[GSM] OTP + ID_ACK both received -> sending auth key request\r\n");
        }
        break;

    case GSM_WAIT_IDENTIFICATION_ACKNOWLEDGE:

        if (identification_acknowledge_received)
        {
            identification_acknowledge_received = false;
            gsm_state = GSM_SEND_AUTH_REQ;
        }
        break;

    case GSM_SEND_AUTH_REQ:

        send_auth_key_request();                  // AKRM_tcas_to_kms[], opens its own QMTPUB session, and publishes it
        gsm_state = GSM_WAIT_AUTH_KEY;
        break;

    case GSM_WAIT_AUTH_KEY:

        if (auth_key_received)                   // WAIT FOR THE AUTH KEY AND IF RECEIVED THEN STORES IT
        {                                        // AND NOW WE GOT THE 2 AUTH KEYS AND CAN USE THAT FOR SENDING
            store_auth_key();                     // copies MQTT_RX_DATA -> AUTHENTICATION_KEY_MESSEGE
            auth_key_received = false;
            gsm_state = GSM_SEND_AUTH_QUERY_MESSEGE;
        }
        break;

    case GSM_SEND_AUTH_QUERY_MESSEGE:
        send_auth_query_messege();
        gsm_state = GSM_WAIT_AUTH_KEY_STATUS_MESSEGE;

        break;

    case GSM_WAIT_AUTH_KEY_STATUS_MESSEGE:
        if (auth_key_status_received)
        {
            store_auth_key_status();
            gsm_state = GSM_DONE;
        }
        break;

    case GSM_DONE:

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

int idx = 0;
void InitConsole(uint32_t sysClk)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART0));

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    UARTStdioConfig(0, 115200, sysClk);
}

void UART7IntHandler(void)
{
    uint32_t status = UARTIntStatus(UART7_BASE, true);
    UARTIntClear(UART7_BASE, status);

    while(UARTCharsAvail(UART7_BASE))
    {
        char c = UARTCharGetNonBlocking(UART7_BASE);

        if(idx < sizeof(rxBuffer)-1)
            rxBuffer[idx++] = c;

        if(c == '\n')
        {
            rxBuffer[idx] = '\0';
            strcpy(current_buffer, rxBuffer);
            line_ready_flag = true;
            idx = 0;
        }
    }
}

int main(void)
{
    sysClk = SysCtlClockFreqSet(
            SYSCTL_USE_PLL | SYSCTL_OSC_MAIN |
            SYSCTL_XTAL_25MHZ | SYSCTL_CFG_VCO_480,
            120000000);

    InitConsole(sysClk);

    // Enable UART7 and GPIO Port C
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART7);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART7));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOC));

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

    // 1ms SysTick, used by the SMS-OTP poller for real time-based timeouts
    SysTickPeriodSet(sysClk / 1000);
    SysTickIntEnable();
    SysTickEnable();

    Pwrkey(false);

    while (1)
    {

        //Jab bhi Smartphone se OTP send karte ho (for testing purpose) then always OTP=1234 and add a new line (enter press kar dena)
        if (line_ready_flag) // FOR CHECKING IF DATA IS AVAILABLE IN THE RX BUFFER
        {
            line_ready_flag = false; // NOW DISABLING THE LINE READY FLAG SO THAT WE CAN KNOW FOR INCOMING DATA NEXT TIME
            gsm_parse_rx(current_buffer); // FOR COPYING THE DATA IN THE CURRENT BUFFER FROM THE RX BUFFER
        }

        modem_task(); // MODEM STATE MACHINE FOR SENDING THE AT COMMANDS FOR SETTING UP CONNECTION

        if (modem_state == MODEM_READY) // CONDITION FOR CHECKING IF THE MODEM IS READY TO TRANSMIT YET
        {
            gsm_protocol_task(); // GSM STATE MACHINE FOR SENDING DATA ON THE CONNECTION
        }
    }
}


