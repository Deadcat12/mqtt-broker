#ifndef SOL_MQTT_H
#define SOL_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTT_MAX_REMAINING_LENGTH 268435455U
#define MQTT_MAX_PACKET_SIZE (2U * 1024U * 1024U)

#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_PUBREC      5
#define MQTT_PUBREL      6
#define MQTT_PUBCOMP     7
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK    11
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

#define MQTT_QOS0 0
#define MQTT_QOS1 1
#define MQTT_QOS2 2

typedef struct {
    char *topic;
    uint8_t qos;
} mqtt_subscription;

typedef struct {
    char *client_id;
    char *username;
    char *password;
    bool has_username;
    bool has_password;
    bool clean_session;
    bool has_will;
    uint8_t will_qos;
    bool will_retain;
    char *will_topic;
    uint8_t *will_payload;
    size_t will_payload_len;
    uint16_t keep_alive;
} mqtt_connect_packet;

typedef struct {
    char *topic;
    uint8_t *payload;
    size_t payload_len;
    uint16_t packet_id;
    uint8_t qos;
    bool dup;
    bool retain;
} mqtt_publish_packet;

typedef struct {
    uint16_t packet_id;
    mqtt_subscription *items;
    size_t count;
} mqtt_subscribe_packet;

typedef struct {
    uint16_t packet_id;
    char **topics;
    size_t count;
} mqtt_unsubscribe_packet;

typedef struct {
    uint16_t packet_id;
} mqtt_ack_packet;

typedef struct {
    uint8_t type;
    uint8_t flags;
    union {
        mqtt_connect_packet connect;
        mqtt_publish_packet publish;
        mqtt_subscribe_packet subscribe;
        mqtt_unsubscribe_packet unsubscribe;
        mqtt_ack_packet ack;
    } body;
} mqtt_packet;

int mqtt_encode_remaining_length(uint8_t *out, size_t len);
int mqtt_decode_remaining_length(const uint8_t *buf, size_t buflen, size_t *value, size_t *used);
int mqtt_parse_packet(const uint8_t *buf, size_t len, mqtt_packet *out, char *err, size_t errlen);
void mqtt_packet_free(mqtt_packet *packet);

uint8_t *mqtt_build_connack(uint8_t session_present, uint8_t return_code, size_t *out_len);
uint8_t *mqtt_build_suback(uint16_t packet_id, const uint8_t *return_codes, size_t count, size_t *out_len);
uint8_t *mqtt_build_unsuback(uint16_t packet_id, size_t *out_len);
uint8_t *mqtt_build_ack(uint8_t type, uint16_t packet_id, size_t *out_len);
uint8_t *mqtt_build_pingresp(size_t *out_len);
uint8_t *mqtt_build_publish(const char *topic, const uint8_t *payload, size_t payload_len,
                            uint8_t qos, bool retain, uint16_t packet_id, size_t *out_len);

bool mqtt_topic_matches(const char *filter, const char *topic);
bool mqtt_topic_filter_is_valid(const char *filter);
bool mqtt_topic_name_is_valid(const char *topic);

#endif
