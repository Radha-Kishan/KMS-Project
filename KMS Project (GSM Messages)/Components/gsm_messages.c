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

void send_identification_message(void)
{
    uint8_t IM_tcas_to_kms[20];  // Identification Message from LTCAS to the KMS
    IM_tcas_to_kms[0] = SOF1;
    IM_tcas_to_kms[1] = SOF2;
    IM_tcas_to_kms[2] = MESSEGE_TYPE_IDENTIFICATION_MESSEGE;
    IM_tcas_to_kms[3] = MESSEGE_LENGTH_LSB_20;
    IM_tcas_to_kms[4] = MESSEGE_LENGTH_MSB;
    IM_tcas_to_kms[5] = year;
    IM_tcas_to_kms[6] = month;
    IM_tcas_to_kms[7] = day;
    IM_tcas_to_kms[8] = hour;
    IM_tcas_to_kms[9] = min;
    IM_tcas_to_kms[10] = sec;
    IM_tcas_to_kms[11] = LOCO_ID;             // 0x22 - LTCAS , 0x11 - STCAS
    IM_tcas_to_kms[12] = TCAS_ID[0];
    IM_tcas_to_kms[13] = TCAS_ID[1];
    IM_tcas_to_kms[14] = TCAS_ID[2];
    IM_tcas_to_kms[15] = SIM_ID;
    IM_tcas_to_kms[16] = CRC;
    IM_tcas_to_kms[17] = CRC;
    IM_tcas_to_kms[18] = CRC;
    IM_tcas_to_kms[19] = CRC;

    mqtt_publish_hex_frame("rk/gsm/messages", IM_tcas_to_kms, 20);
}

void store_identification_acknowledge(void)
{
    int len = (MQTT_RX_LEN < sizeof(IDENTIFICATION_ACKNOWLEDGE_MESSEGE))
                ? MQTT_RX_LEN : sizeof(IDENTIFICATION_ACKNOWLEDGE_MESSEGE);
    memcpy(IDENTIFICATION_ACKNOWLEDGE_MESSEGE, MQTT_RX_DATA, len);
}

void send_auth_key_request(void)
{
    uint8_t AKRM_tcas_to_kms[24]; // Authentication Key Request Messege from ltcas to the kms
    AKRM_tcas_to_kms[0] = SOF1;
    AKRM_tcas_to_kms[1] = SOF2;
    AKRM_tcas_to_kms[2] = MESSEGE_TYPE_AUTH_KEY_REQ_MESSEGE;
    AKRM_tcas_to_kms[3] = MESSEGE_LENGTH_LSB_24;
    AKRM_tcas_to_kms[4] = MESSEGE_LENGTH_MSB;
    AKRM_tcas_to_kms[5] = year;
    AKRM_tcas_to_kms[6] = month;
    AKRM_tcas_to_kms[7] = day;
    AKRM_tcas_to_kms[8] = hour;
    AKRM_tcas_to_kms[9] = min;
    AKRM_tcas_to_kms[10] = sec;
    AKRM_tcas_to_kms[11] = LOCO_ID;             // 0x22 - LTCAS , 0x11 - STCAS
    AKRM_tcas_to_kms[12] = TCAS_ID[0];
    AKRM_tcas_to_kms[13] = TCAS_ID[1];
    AKRM_tcas_to_kms[14] = TCAS_ID[2];
    AKRM_tcas_to_kms[15] = SIM_ID;
    AKRM_tcas_to_kms[16] = OTP[0];
    AKRM_tcas_to_kms[17] = OTP[1];
    AKRM_tcas_to_kms[18] = OTP[2];
    AKRM_tcas_to_kms[19] = OTP[3];
    AKRM_tcas_to_kms[20] = CRC;
    AKRM_tcas_to_kms[21] = CRC;
    AKRM_tcas_to_kms[22] = CRC;
    AKRM_tcas_to_kms[23] = CRC;

    mqtt_publish_hex_frame("rk/gsm/messages", AKRM_tcas_to_kms, 24);
}

void store_auth_key(void)
{
    int len = (MQTT_RX_LEN < sizeof(AUTHENTICATION_KEY_MESSEGE))
                ? MQTT_RX_LEN : sizeof(AUTHENTICATION_KEY_MESSEGE);
    memcpy(AUTHENTICATION_KEY_MESSEGE, MQTT_RX_DATA, len);
}

void send_auth_query_messege(void)
{
    uint8_t AQM_tcas_to_kms[19]; // AUTHENTICATION QUERY Message from ltcas to the kms
    AQM_tcas_to_kms[0] = SOF1;
    AQM_tcas_to_kms[1] = SOF2;
    AQM_tcas_to_kms[2] = MESSEGE_TYPE_AUTH_QUERY_MESSEGE;
    AQM_tcas_to_kms[3] = MESSEGE_LENGTH_LSB_19;
    AQM_tcas_to_kms[4] = MESSEGE_LENGTH_MSB;
    AQM_tcas_to_kms[5] = year;
    AQM_tcas_to_kms[6] = month;
    AQM_tcas_to_kms[7] = day;
    AQM_tcas_to_kms[8] = hour;
    AQM_tcas_to_kms[9] = min;
    AQM_tcas_to_kms[10] = sec;
    AQM_tcas_to_kms[11] = LOCO_ID;             // 0x22 - LTCAS , 0x11 - STCAS
    AQM_tcas_to_kms[12] = TCAS_ID[0];
    AQM_tcas_to_kms[13] = TCAS_ID[1];
    AQM_tcas_to_kms[14] = TCAS_ID[2];
    AQM_tcas_to_kms[15] = CRC;
    AQM_tcas_to_kms[16] = CRC;
    AQM_tcas_to_kms[17] = CRC;
    AQM_tcas_to_kms[18] = CRC;

    mqtt_publish_hex_frame("rk/gsm/messages", AQM_tcas_to_kms, 19);
}

void store_auth_key_status(void)
{
    int len = (MQTT_RX_LEN < sizeof(AUTHENTICATION_KEY_STATUS_MESSEGE))
                ? MQTT_RX_LEN : sizeof(AUTHENTICATION_KEY_STATUS_MESSEGE);
    memcpy(AUTHENTICATION_KEY_STATUS_MESSEGE, MQTT_RX_DATA, len);
}
