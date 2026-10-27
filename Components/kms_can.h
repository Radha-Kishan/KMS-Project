#ifndef KMS_CAN_H
#define KMS_CAN_H

#include <stdint.h>
#include <stdbool.h>
#include "Components/kms_keys.h"

// ============================================================
// CAN IDs - 29-bit extended, matching the Hercules (can_if.c)
// Hercules -> Tiva IDs are base + CPU offset (0..3), so the RX
// filters ignore the 2 low ID bits.
// ============================================================
#define CPU_STARTUP_CAN_ID          0x080U
#define CPU_TIME_CAN_ID             0x100U      // bytes 0-3 GPS seconds (LE), byte 4 flags (bit0 = valid)
#define CPU_HEARTBEAT_CAN_ID        0x110U
#define KMS_START_CAN_ID            0x130U      // any frame here starts the KMS process
#define CPU_UNIVERSAL_ACK_CAN_ID    0x160U      // Hercules ACK: bytes 0-1 = acked CAN ID, byte 3 = status
#define CPU_ID_RANGE_MASK           0x1FFFFFFCU

// Tiva -> Hercules ("KMS" entry of the can_manager.c device table)
#define KMS_STARTUP_ACK_CAN_ID      0x08AU
#define KMS_HEARTBEAT_ACK_CAN_ID    0x0C7U
#define KMS_KEYS_TX_CAN_ID          0x134U      // received by kms_rx_handle() on the Hercules
#define KMS_START_ACK_CAN_ID        0x138U      // ACK for 0x130 - needs an RX mailbox + handler on the Hercules

// Start request ACK (universal ACK layout, as send_cpu_universal_ack() on the Hercules)
// Byte 0-1 : CAN ID being acknowledged (MSB first), byte 2 : command (byte 0 of the 0x130 frame)
// Byte 3   : status, bytes 4-7 : 0
#define KMS_START_ACK_OK            0x00U
#define KMS_START_ACK_INVALID       0x01U       // unknown command byte

// Message types (can_if.h / kms.c on the Hercules)
#define MSG_TYPE_CPU_STARTUP        0x01U
#define MSG_TYPE_PERIPH_ACK         0x81U
#define MSG_TYPE_CPU_HEARTBEAT      0x0110U     // bytes 0-1, little-endian
#define MSG_TYPE_HB_ACK             0x0111U     // bytes 0-1, little-endian
#define KMS_PKT_TYPE_AUTH_KEYS      0x03U       // kms.c: get_authentication_keys()

#define KMS_FRAGMENT_PAYLOAD_BYTES  6U
#define KMS_HERC_ACK_TIMEOUT_MS     500U
#define KMS_HERC_MAX_RETRIES        3U

void kms_can_init(uint32_t firmware_checksum);
void CAN0IntHandler(void);

// true once after the Hercules sent a frame on 0x130 (cleared by reading)
bool kms_can_start_requested(void);

// GPS seconds from the latest valid CPU time frame, advanced by the local tick
bool kms_can_get_cpu_time(uint32_t *gps_sec);

// Sends one key set to the Hercules (type 0x03, 7 fragments), retried until ACKed
void kms_can_send_key_set(const kms_key_set_t *set);
void kms_can_task(void);

extern volatile uint8_t  kms_last_start_cmd;            // byte 0 of the last 0x130 frame (debug)
extern volatile uint32_t kms_herc_tx_failed_count;      // key sets not ACKed after all retries

#endif
