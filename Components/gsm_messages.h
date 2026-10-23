#ifndef GSM_MESSAGES_H
#define GSM_MESSAGES_H

#include <stdint.h>
#include <stdbool.h>

// Messages to the KMS (SRS Amdt-3, 4.7.8)
void send_identification_message(void);                // 0x90
void send_auth_key_request(const uint8_t otp[4]);       // 0x92, OTP as 4 ASCII characters
void send_auth_query_messege(void);                     // 0x94

// Results of the KMS replies, set by kms_parse_message()
typedef struct
{
    bool    id_ack_received;        // 0x91
    uint8_t id_ack_status;          // KMS_ACK_OTP_SENT / ..._ID_NOT_REGISTERED / ..._DELIVERY_FAILED

    bool    keys_received;          // 0x93, already stored in EEPROM
    bool    keys_store_failed;

    bool    status_received;        // 0x95
    uint8_t status_unique_id[4];    // Key Set Unique ID currently in the KMS
} kms_rx_result_t;

extern kms_rx_result_t kms_rx;

// Checks and handles one complete binary message from the KMS
void kms_parse_message(const uint8_t *msg, uint16_t len);

#endif
