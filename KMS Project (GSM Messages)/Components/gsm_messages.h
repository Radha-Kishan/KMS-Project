#ifndef GSM_MESSAGES_H
#define GSM_MESSAGES_H

void identification_ack_message(void);
void send_auth_query_messege(void);
void store_auth_key(void);
void send_auth_key_request(void);
void send_identification_message(void);
void store_identification_acknowledge(void);
void store_auth_key_status(void);
static void mqtt_publish_hex_frame(const char *topic, uint8_t *data, uint8_t len);

#endif
