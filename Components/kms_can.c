#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_can.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/interrupt.h"
#include "driverlib/can.h"

#include "main.h"
#include "Components/kms_can.h"

// ============================================================
// CAN0 MESSAGE OBJECTS (1 = highest priority)
//
// TX objects are loaded only when something is sent and never use
// MSG_OBJ_USE_ID_FILTER - with a mask they would also accept
// incoming frames and steal them from the RX objects.
// ============================================================
#define OBJ_STARTUP_RX          1U
#define OBJ_HEARTBEAT_RX        2U
#define OBJ_KMS_START_RX        3U
#define OBJ_CPU_TIME_RX         4U
#define OBJ_HERC_ACK_RX         5U
#define OBJ_STARTUP_ACK_TX      6U
#define OBJ_HEARTBEAT_ACK_TX    7U
#define OBJ_KEYS_TX             8U
#define OBJ_START_ACK_TX        9U

static uint32_t g_firmware_checksum;

// Start request
static volatile bool start_requested = false;
volatile uint8_t kms_last_start_cmd = 0U;

// CPU time
static volatile bool     cpu_time_valid = false;
static volatile uint32_t cpu_time_sec = 0U;
static volatile uint32_t cpu_time_rx_ms = 0U;

// Key set transmission to the Hercules
static uint8_t  keys_tx_buf[KMS_KEY_SET_BYTES];
static bool     keys_tx_waiting_ack = false;
static uint8_t  keys_tx_retry_count = 0U;
static uint32_t keys_tx_sent_ms = 0U;
static volatile bool keys_tx_ack_received = false;
volatile uint32_t kms_herc_tx_failed_count = 0U;

// ============================================================
// CAN TX
// ============================================================

static void can_load_tx(uint32_t obj, uint32_t can_id, uint8_t *data)
{
    tCANMsgObject txMsg;

    txMsg.ui32MsgID = can_id;
    txMsg.ui32MsgIDMask = 0U;
    txMsg.ui32Flags = MSG_OBJ_EXTENDED_ID;
    txMsg.ui32MsgLen = 8U;
    txMsg.pui8MsgData = data;

    CANMessageSet(CAN0_BASE, obj, &txMsg, MSG_OBJ_TYPE_TX);
}

// From the main loop: the CAN interrupt also uses the IF1 registers
// (startup / heartbeat replies), so it is blocked while loading the object.
// Waits until the frame has left, so the next fragment does not overwrite it.
static void can_send_blocking(uint32_t obj, uint32_t can_id, uint8_t *data)
{
    uint32_t timeout = 100000U;

    IntDisable(INT_CAN0);
    can_load_tx(obj, can_id, data);
    IntEnable(INT_CAN0);

    while ((CANStatusGet(CAN0_BASE, CAN_STS_TXREQUEST) & (1UL << (obj - 1U))) && --timeout)
    {
    }
}

// ============================================================
// STARTUP / HEARTBEAT (same as RFID / NMS)
// ============================================================

static void handle_startup(const uint8_t *data)
{
    static uint8_t ack[8];

    if (data[0] != MSG_TYPE_CPU_STARTUP)
    {
        return;
    }

    memset(ack, 0, sizeof(ack));
    ack[0] = MSG_TYPE_PERIPH_ACK;
    ack[1] = (uint8_t)(g_firmware_checksum >> 24);
    ack[2] = (uint8_t)(g_firmware_checksum >> 16);
    ack[3] = (uint8_t)(g_firmware_checksum >> 8);
    ack[4] = (uint8_t)(g_firmware_checksum);

    can_load_tx(OBJ_STARTUP_ACK_TX, KMS_STARTUP_ACK_CAN_ID, ack);
}

static void handle_heartbeat(const uint8_t *data)
{
    static uint8_t ack[8];
    uint16_t msg_type = (uint16_t)data[0] | ((uint16_t)data[1] << 8);

    if (msg_type != MSG_TYPE_CPU_HEARTBEAT)
    {
        return;
    }

    memset(ack, 0, sizeof(ack));
    ack[0] = (uint8_t)(MSG_TYPE_HB_ACK & 0xFFU);
    ack[1] = (uint8_t)(MSG_TYPE_HB_ACK >> 8);

    can_load_tx(OBJ_HEARTBEAT_ACK_TX, KMS_HEARTBEAT_ACK_CAN_ID, ack);
}

// ACK for every 0x130 frame. rx_id is the received ID (0x130 + CPU offset),
// so the Hercules can see which of its frames was acknowledged.
static void handle_start_request(uint32_t rx_id, const uint8_t *data)
{
    static uint8_t ack[8];
    uint8_t cmd = data[0];

    kms_last_start_cmd = cmd;

    memset(ack, 0, sizeof(ack));
    ack[0] = (uint8_t)(rx_id >> 8);
    ack[1] = (uint8_t)(rx_id & 0xFFU); 
    ack[2] = cmd;

    // 0x01 query / 0x02 identification / 0x03 key request (kms.c on the Hercules)
    if ((cmd >= 0x01U) && (cmd <= 0x03U))
    {
        ack[3] = KMS_START_ACK_OK;
        start_requested = true;
    }
    else
    {
        ack[3] = KMS_START_ACK_INVALID;
    }

    can_load_tx(OBJ_START_ACK_TX, KMS_START_ACK_CAN_ID, ack);
}

// ============================================================
// CAN0 INTERRUPT
// ============================================================

void CAN0IntHandler(void)
{
    uint32_t cause;
    uint8_t data[8];
    tCANMsgObject rxMsg;

    while ((cause = CANIntStatus(CAN0_BASE, CAN_INT_STS_CAUSE)) != 0U)
    {
        if (cause == CAN_INT_INTID_STATUS)
        {
            (void)CANStatusGet(CAN0_BASE, CAN_STS_CONTROL);
            continue;
        }

        rxMsg.pui8MsgData = data;
        CANMessageGet(CAN0_BASE, cause, &rxMsg, true);

        if (!(rxMsg.ui32Flags & MSG_OBJ_NEW_DATA))
        {
            continue;
        }

        switch (cause)
        {
        case OBJ_STARTUP_RX:
            handle_startup(data);
            break;

        case OBJ_HEARTBEAT_RX:
            handle_heartbeat(data);
            break;

        case OBJ_KMS_START_RX:
            // A valid command on 0x130 starts the whole KMS process; every frame is ACKed
            handle_start_request(rxMsg.ui32MsgID, data);
            break;

        case OBJ_CPU_TIME_RX:
            if (data[4] & 0x01U)
            {
                cpu_time_sec = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
                cpu_time_rx_ms = g_msTicks;
                cpu_time_valid = true;
            }
            break;

        case OBJ_HERC_ACK_RX:
            // Bytes 0-1 = CAN ID being acknowledged, byte 3 = status (0 = OK)
            if ((data[0] == (uint8_t)(KMS_KEYS_TX_CAN_ID >> 8)) &&
                (data[1] == (uint8_t)(KMS_KEYS_TX_CAN_ID & 0xFFU)) &&
                (data[3] == 0x00U))
            {
                keys_tx_ack_received = true;
            }
            break;

        default:
            break;
        }
    }
}

// ============================================================
// API
// ============================================================

bool kms_can_start_requested(void)
{
    if (!start_requested)
    {
        return false;
    }
    start_requested = false;
    return true;
}

bool kms_can_get_cpu_time(uint32_t *gps_sec)
{
    uint32_t sec, rx_ms;

    if (!cpu_time_valid)
    {
        return false;
    }

    IntDisable(INT_CAN0);
    sec = cpu_time_sec;
    rx_ms = cpu_time_rx_ms;
    IntEnable(INT_CAN0);

    *gps_sec = sec + (g_msTicks - rx_ms) / 1000U;
    return true;
}

// Fragments as built by nms_build_fragment() / read by kms_rx_handle():
// Byte 0   : seq_total[3:0] << 4 | pkt_type[3:0]
// Byte 1   : seq_index[5:0] << 2 | seq_total[5:4]
// Byte 2-7 : 6 payload bytes
static void send_key_set_fragments(void)
{
    uint8_t frame[8];
    uint8_t total = (uint8_t)((KMS_KEY_SET_BYTES + KMS_FRAGMENT_PAYLOAD_BYTES - 1U) / KMS_FRAGMENT_PAYLOAD_BYTES);
    uint8_t index;
    uint8_t i;
    uint16_t offset;

    for (index = 0U; index < total; index++)
    {
        memset(frame, 0, sizeof(frame));
        frame[0] = (uint8_t)(((total & 0x0FU) << 4) | (KMS_PKT_TYPE_AUTH_KEYS & 0x0FU));
        frame[1] = (uint8_t)(((index & 0x3FU) << 2) | ((total >> 4) & 0x03U));

        offset = (uint16_t)index * KMS_FRAGMENT_PAYLOAD_BYTES;
        for (i = 0U; i < KMS_FRAGMENT_PAYLOAD_BYTES; i++)
        {
            if ((offset + i) < KMS_KEY_SET_BYTES)
            {
                frame[2U + i] = keys_tx_buf[offset + i];
            }
        }

        can_send_blocking(OBJ_KEYS_TX, KMS_KEYS_TX_CAN_ID, frame);
    }

    keys_tx_sent_ms = g_msTicks;
}

void kms_can_send_key_set(const kms_key_set_t *set)
{
    if (set == 0)
    {
        return;
    }

    memcpy(keys_tx_buf, set, KMS_KEY_SET_BYTES);
    keys_tx_ack_received = false;
    keys_tx_retry_count = 0U;
    keys_tx_waiting_ack = true;

    send_key_set_fragments();
}

// Resends the key set until the Hercules ACKs it (like the RFID retry logic)
void kms_can_task(void)
{
    if (!keys_tx_waiting_ack)
    {
        return;
    }

    if (keys_tx_ack_received)
    {
        keys_tx_waiting_ack = false;
        return;
    }

    if ((g_msTicks - keys_tx_sent_ms) < KMS_HERC_ACK_TIMEOUT_MS)
    {
        return;
    }

    if (keys_tx_retry_count < KMS_HERC_MAX_RETRIES)
    {
        keys_tx_retry_count++;
        send_key_set_fragments();
    }
    else
    {
        kms_herc_tx_failed_count++;
        keys_tx_waiting_ack = false;
    }
}

// ============================================================
// CAN0 INIT - PA0 = CAN0RX, PA1 = CAN0TX, 500 kbit/s
// ============================================================

static void set_rx_object(uint32_t obj, uint32_t can_id)
{
    tCANMsgObject rxMsg;
    uint8_t rxData[8];

    rxMsg.ui32MsgID = can_id;
    rxMsg.ui32MsgIDMask = CPU_ID_RANGE_MASK;
    rxMsg.ui32Flags = MSG_OBJ_RX_INT_ENABLE | MSG_OBJ_USE_EXT_FILTER | MSG_OBJ_EXTENDED_ID;
    rxMsg.ui32MsgLen = 8U;
    rxMsg.pui8MsgData = rxData;

    CANMessageSet(CAN0_BASE, obj, &rxMsg, MSG_OBJ_TYPE_RX);
}

void kms_can_init(uint32_t firmware_checksum)
{
    g_firmware_checksum = firmware_checksum;

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_CAN0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_CAN0));

    GPIOPinConfigure(GPIO_PA0_CAN0RX);
    GPIOPinConfigure(GPIO_PA1_CAN0TX);
    GPIOPinTypeCAN(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    CANInit(CAN0_BASE);
    CANBitRateSet(CAN0_BASE, sysClk, 500000);

    set_rx_object(OBJ_STARTUP_RX,   CPU_STARTUP_CAN_ID);
    set_rx_object(OBJ_HEARTBEAT_RX, CPU_HEARTBEAT_CAN_ID);
    set_rx_object(OBJ_KMS_START_RX, KMS_START_CAN_ID);
    set_rx_object(OBJ_CPU_TIME_RX,  CPU_TIME_CAN_ID);
    set_rx_object(OBJ_HERC_ACK_RX,  CPU_UNIVERSAL_ACK_CAN_ID);

    // NOTE: no CANMessageSet() for the TX objects here - MSG_OBJ_TYPE_TX
    // transmits immediately. They are loaded only when something is sent.

    CANIntEnable(CAN0_BASE, CAN_INT_MASTER);
    IntEnable(INT_CAN0);

    CANEnable(CAN0_BASE);
}
