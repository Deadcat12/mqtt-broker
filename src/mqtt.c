#define _POSIX_C_SOURCE 200809L
#include "mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_u16(const uint8_t **p) {
    uint16_t v = (uint16_t)(((*p)[0] << 8) | (*p)[1]);
    *p += 2;
    return v;
}

static void write_u16(uint8_t **p, uint16_t v) {
    (*p)[0] = (uint8_t)((v >> 8) & 0xFFU);
    (*p)[1] = (uint8_t)(v & 0xFFU);
    *p += 2;
}

static char *read_utf8_string(const uint8_t **p, const uint8_t *end, uint16_t *out_len) {
    if ((size_t)(end - *p) < 2U) {
        return NULL;
    }
    uint16_t len = read_u16(p);
    if ((size_t)(end - *p) < len) {
        return NULL;
    }
    char *s = (char *)calloc((size_t)len + 1U, 1U);
    if (!s) {
        return NULL;
    }
    memcpy(s, *p, len);
    *p += len;
    if (out_len) {
        *out_len = len;
    }
    return s;
}

static uint8_t *read_binary_field(const uint8_t **p, const uint8_t *end, size_t *out_len) {
    if ((size_t)(end - *p) < 2U) {
        return NULL;
    }
    uint16_t len = read_u16(p);
    if ((size_t)(end - *p) < len) {
        return NULL;
    }
    uint8_t *b = NULL;
    if (len > 0U) {
        b = (uint8_t *)malloc(len);
        if (!b) {
            return NULL;
        }
        memcpy(b, *p, len);
    } else {
        b = (uint8_t *)calloc(1U, 1U);
        if (!b) {
            return NULL;
        }
    }
    *p += len;
    *out_len = len;
    return b;
}

static int append_remaining(uint8_t **p, size_t len) {
    uint8_t tmp[4];
    int used = mqtt_encode_remaining_length(tmp, len);
    if (used <= 0) {
        return -1;
    }
    memcpy(*p, tmp, (size_t)used);
    *p += used;
    return used;
}

int mqtt_encode_remaining_length(uint8_t *out, size_t len) {
    if (!out || len > MQTT_MAX_REMAINING_LENGTH) {
        return -1;
    }
    int used = 0;
    do {
        uint8_t encoded = (uint8_t)(len % 128U);
        len /= 128U;
        if (len > 0U) {
            encoded |= 128U;
        }
        out[used++] = encoded;
        if (used > 4) {
            return -1;
        }
    } while (len > 0U);
    return used;
}

int mqtt_decode_remaining_length(const uint8_t *buf, size_t buflen, size_t *value, size_t *used) {
    if (!buf || !value || !used) {
        return -1;
    }
    size_t multiplier = 1U;
    size_t decoded = 0U;
    size_t i = 0U;
    uint8_t encoded = 0U;
    do {
        if (i >= buflen || i >= 4U) {
            return -1;
        }
        encoded = buf[i++];
        decoded += (size_t)(encoded & 127U) * multiplier;
        if (multiplier > 128U * 128U * 128U) {
            return -1;
        }
        multiplier *= 128U;
    } while ((encoded & 128U) != 0U);
    *value = decoded;
    *used = i;
    return 0;
}

static int parse_connect(const uint8_t *payload, size_t payload_len, mqtt_packet *out, char *err, size_t errlen) {
    const uint8_t *p = payload;
    const uint8_t *end = payload + payload_len;
    uint16_t proto_len = 0U;
    char *proto_name = read_utf8_string(&p, end, &proto_len);
    if (!proto_name) {
        snprintf(err, errlen, "CONNECT: cannot read protocol name");
        return -1;
    }
    if (proto_len != 4U || memcmp(proto_name, "MQTT", 4U) != 0) {
        free(proto_name);
        snprintf(err, errlen, "CONNECT: unsupported protocol name");
        return -1;
    }
    free(proto_name);
    if ((size_t)(end - p) < 4U) {
        snprintf(err, errlen, "CONNECT: variable header is truncated");
        return -1;
    }
    uint8_t level = *p++;
    if (level != 4U) {
        snprintf(err, errlen, "CONNECT: only MQTT 3.1.1 protocol level 4 is supported");
        return -1;
    }
    uint8_t flags = *p++;
    if ((flags & 0x01U) != 0U) {
        snprintf(err, errlen, "CONNECT: reserved flag is set");
        return -1;
    }
    bool username = (flags & 0x80U) != 0U;
    bool password = (flags & 0x40U) != 0U;
    bool will_retain = (flags & 0x20U) != 0U;
    uint8_t will_qos = (uint8_t)((flags >> 3U) & 0x03U);
    bool will = (flags & 0x04U) != 0U;
    bool clean_session = (flags & 0x02U) != 0U;
    if (!will && (will_qos != 0U || will_retain)) {
        snprintf(err, errlen, "CONNECT: will flags are inconsistent");
        return -1;
    }
    if (will_qos == 3U) {
        snprintf(err, errlen, "CONNECT: invalid will QoS");
        return -1;
    }
    uint16_t keep_alive = read_u16(&p);
    uint16_t client_id_len = 0U;
    char *client_id = read_utf8_string(&p, end, &client_id_len);
    if (!client_id) {
        snprintf(err, errlen, "CONNECT: cannot read client id");
        return -1;
    }
    if (client_id_len == 0U) {
        free(client_id);
        snprintf(err, errlen, "CONNECT: empty Client ID is not accepted by this broker");
        return -1;
    }
    mqtt_connect_packet *c = &out->body.connect;
    c->client_id = client_id;
    c->clean_session = clean_session;
    c->has_username = username;
    c->has_password = password;
    c->has_will = will;
    c->will_qos = will_qos;
    c->will_retain = will_retain;
    c->keep_alive = keep_alive;
    if (will) {
        c->will_topic = read_utf8_string(&p, end, NULL);
        c->will_payload = read_binary_field(&p, end, &c->will_payload_len);
        if (!c->will_topic || !c->will_payload) {
            snprintf(err, errlen, "CONNECT: cannot read will topic/message");
            return -1;
        }
    }
    if (username) {
        c->username = read_utf8_string(&p, end, NULL);
        if (!c->username) {
            snprintf(err, errlen, "CONNECT: cannot read username");
            return -1;
        }
    }
    if (password) {
        c->password = read_utf8_string(&p, end, NULL);
        if (!c->password) {
            snprintf(err, errlen, "CONNECT: cannot read password");
            return -1;
        }
    }
    if (p != end) {
        snprintf(err, errlen, "CONNECT: trailing bytes in packet");
        return -1;
    }
    return 0;
}

static int parse_publish(const uint8_t *payload, size_t payload_len, mqtt_packet *out, char *err, size_t errlen) {
    const uint8_t *p = payload;
    const uint8_t *end = payload + payload_len;
    uint16_t topic_len = 0U;
    char *topic = read_utf8_string(&p, end, &topic_len);
    if (!topic || topic_len == 0U) {
        free(topic);
        snprintf(err, errlen, "PUBLISH: cannot read topic name");
        return -1;
    }
    if (!mqtt_topic_name_is_valid(topic)) {
        free(topic);
        snprintf(err, errlen, "PUBLISH: topic name contains wildcard");
        return -1;
    }
    uint8_t qos = (uint8_t)((out->flags >> 1U) & 0x03U);
    if (qos == 3U) {
        free(topic);
        snprintf(err, errlen, "PUBLISH: invalid QoS");
        return -1;
    }
    uint16_t packet_id = 0U;
    if (qos > MQTT_QOS0) {
        if ((size_t)(end - p) < 2U) {
            free(topic);
            snprintf(err, errlen, "PUBLISH: missing packet id");
            return -1;
        }
        packet_id = read_u16(&p);
    }
    size_t rest = (size_t)(end - p);
    uint8_t *data = NULL;
    if (rest > 0U) {
        data = (uint8_t *)malloc(rest);
        if (!data) {
            free(topic);
            snprintf(err, errlen, "PUBLISH: out of memory");
            return -1;
        }
        memcpy(data, p, rest);
    } else {
        data = (uint8_t *)calloc(1U, 1U);
        if (!data) {
            free(topic);
            snprintf(err, errlen, "PUBLISH: out of memory");
            return -1;
        }
    }
    out->body.publish.topic = topic;
    out->body.publish.payload = data;
    out->body.publish.payload_len = rest;
    out->body.publish.packet_id = packet_id;
    out->body.publish.qos = qos;
    out->body.publish.dup = (out->flags & 0x08U) != 0U;
    out->body.publish.retain = (out->flags & 0x01U) != 0U;
    return 0;
}

static int parse_subscribe(const uint8_t *payload, size_t payload_len, mqtt_packet *out, char *err, size_t errlen) {
    const uint8_t *p = payload;
    const uint8_t *end = payload + payload_len;
    if ((out->flags & 0x0FU) != 0x02U) {
        snprintf(err, errlen, "SUBSCRIBE: invalid fixed header flags");
        return -1;
    }
    if ((size_t)(end - p) < 2U) {
        snprintf(err, errlen, "SUBSCRIBE: missing packet id");
        return -1;
    }
    out->body.subscribe.packet_id = read_u16(&p);
    size_t cap = 4U;
    mqtt_subscription *items = (mqtt_subscription *)calloc(cap, sizeof(*items));
    if (!items) {
        snprintf(err, errlen, "SUBSCRIBE: out of memory");
        return -1;
    }
    size_t count = 0U;
    while (p < end) {
        if (count == cap) {
            cap *= 2U;
            mqtt_subscription *tmp = (mqtt_subscription *)realloc(items, cap * sizeof(*items));
            if (!tmp) {
                free(items);
                snprintf(err, errlen, "SUBSCRIBE: out of memory");
                return -1;
            }
            items = tmp;
        }
        char *topic = read_utf8_string(&p, end, NULL);
        if (!topic || p >= end) {
            free(topic);
            for (size_t i = 0U; i < count; ++i) {
                free(items[i].topic);
            }
            free(items);
            snprintf(err, errlen, "SUBSCRIBE: malformed topic/qos tuple");
            return -1;
        }
        uint8_t qos = *p++;
        items[count].topic = topic;
        items[count].qos = (qos <= MQTT_QOS2 && mqtt_topic_filter_is_valid(topic)) ? qos : 0x80U;
        count++;
    }
    if (count == 0U) {
        free(items);
        snprintf(err, errlen, "SUBSCRIBE: empty topic list");
        return -1;
    }
    out->body.subscribe.items = items;
    out->body.subscribe.count = count;
    return 0;
}

static int parse_unsubscribe(const uint8_t *payload, size_t payload_len, mqtt_packet *out, char *err, size_t errlen) {
    const uint8_t *p = payload;
    const uint8_t *end = payload + payload_len;
    if ((out->flags & 0x0FU) != 0x02U) {
        snprintf(err, errlen, "UNSUBSCRIBE: invalid fixed header flags");
        return -1;
    }
    if ((size_t)(end - p) < 2U) {
        snprintf(err, errlen, "UNSUBSCRIBE: missing packet id");
        return -1;
    }
    out->body.unsubscribe.packet_id = read_u16(&p);
    size_t cap = 4U;
    char **topics = (char **)calloc(cap, sizeof(*topics));
    if (!topics) {
        snprintf(err, errlen, "UNSUBSCRIBE: out of memory");
        return -1;
    }
    size_t count = 0U;
    while (p < end) {
        if (count == cap) {
            cap *= 2U;
            char **tmp = (char **)realloc(topics, cap * sizeof(*topics));
            if (!tmp) {
                free(topics);
                snprintf(err, errlen, "UNSUBSCRIBE: out of memory");
                return -1;
            }
            topics = tmp;
        }
        topics[count] = read_utf8_string(&p, end, NULL);
        if (!topics[count]) {
            for (size_t i = 0U; i < count; ++i) {
                free(topics[i]);
            }
            free(topics);
            snprintf(err, errlen, "UNSUBSCRIBE: malformed topic filter");
            return -1;
        }
        count++;
    }
    if (count == 0U) {
        free(topics);
        snprintf(err, errlen, "UNSUBSCRIBE: empty topic list");
        return -1;
    }
    out->body.unsubscribe.topics = topics;
    out->body.unsubscribe.count = count;
    return 0;
}

static int parse_ack(const uint8_t *payload, size_t payload_len, mqtt_packet *out, char *err, size_t errlen) {
    if (payload_len != 2U) {
        snprintf(err, errlen, "ACK: expected two-byte packet id");
        return -1;
    }
    const uint8_t *p = payload;
    out->body.ack.packet_id = read_u16(&p);
    return 0;
}

int mqtt_parse_packet(const uint8_t *buf, size_t len, mqtt_packet *out, char *err, size_t errlen) {
    if (!buf || !out || len < 2U) {
        snprintf(err, errlen, "packet is too short");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->type = (uint8_t)(buf[0] >> 4U);
    out->flags = (uint8_t)(buf[0] & 0x0FU);
    size_t remaining = 0U;
    size_t used = 0U;
    if (mqtt_decode_remaining_length(buf + 1U, len - 1U, &remaining, &used) != 0) {
        snprintf(err, errlen, "cannot decode remaining length");
        return -1;
    }
    if (1U + used + remaining != len) {
        snprintf(err, errlen, "packet length mismatch");
        return -1;
    }
    const uint8_t *payload = buf + 1U + used;
    switch (out->type) {
        case MQTT_CONNECT:
            if (out->flags != 0U) {
                snprintf(err, errlen, "CONNECT: invalid flags");
                return -1;
            }
            return parse_connect(payload, remaining, out, err, errlen);
        case MQTT_PUBLISH:
            return parse_publish(payload, remaining, out, err, errlen);
        case MQTT_SUBSCRIBE:
            return parse_subscribe(payload, remaining, out, err, errlen);
        case MQTT_UNSUBSCRIBE:
            return parse_unsubscribe(payload, remaining, out, err, errlen);
        case MQTT_PUBACK:
        case MQTT_PUBREC:
        case MQTT_PUBREL:
        case MQTT_PUBCOMP:
            return parse_ack(payload, remaining, out, err, errlen);
        case MQTT_PINGREQ:
        case MQTT_DISCONNECT:
            if (remaining != 0U) {
                snprintf(err, errlen, "packet type %u must have zero remaining length", out->type);
                return -1;
            }
            return 0;
        default:
            snprintf(err, errlen, "unsupported packet type %u", out->type);
            return -1;
    }
}

void mqtt_packet_free(mqtt_packet *packet) {
    if (!packet) {
        return;
    }
    switch (packet->type) {
        case MQTT_CONNECT:
            free(packet->body.connect.client_id);
            free(packet->body.connect.username);
            free(packet->body.connect.password);
            free(packet->body.connect.will_topic);
            free(packet->body.connect.will_payload);
            break;
        case MQTT_PUBLISH:
            free(packet->body.publish.topic);
            free(packet->body.publish.payload);
            break;
        case MQTT_SUBSCRIBE:
            for (size_t i = 0U; i < packet->body.subscribe.count; ++i) {
                free(packet->body.subscribe.items[i].topic);
            }
            free(packet->body.subscribe.items);
            break;
        case MQTT_UNSUBSCRIBE:
            for (size_t i = 0U; i < packet->body.unsubscribe.count; ++i) {
                free(packet->body.unsubscribe.topics[i]);
            }
            free(packet->body.unsubscribe.topics);
            break;
        default:
            break;
    }
    memset(packet, 0, sizeof(*packet));
}

uint8_t *mqtt_build_connack(uint8_t session_present, uint8_t return_code, size_t *out_len) {
    *out_len = 4U;
    uint8_t *buf = (uint8_t *)malloc(*out_len);
    if (!buf) {
        return NULL;
    }
    buf[0] = (uint8_t)(MQTT_CONNACK << 4U);
    buf[1] = 2U;
    buf[2] = (uint8_t)(session_present & 0x01U);
    buf[3] = return_code;
    return buf;
}

uint8_t *mqtt_build_suback(uint16_t packet_id, const uint8_t *return_codes, size_t count, size_t *out_len) {
    size_t remaining = 2U + count;
    uint8_t rlb[4];
    int rllen = mqtt_encode_remaining_length(rlb, remaining);
    if (rllen <= 0) {
        return NULL;
    }
    *out_len = 1U + (size_t)rllen + remaining;
    uint8_t *buf = (uint8_t *)malloc(*out_len);
    if (!buf) {
        return NULL;
    }
    uint8_t *p = buf;
    *p++ = (uint8_t)(MQTT_SUBACK << 4U);
    memcpy(p, rlb, (size_t)rllen);
    p += rllen;
    write_u16(&p, packet_id);
    memcpy(p, return_codes, count);
    return buf;
}

uint8_t *mqtt_build_unsuback(uint16_t packet_id, size_t *out_len) {
    return mqtt_build_ack(MQTT_UNSUBACK, packet_id, out_len);
}

uint8_t *mqtt_build_ack(uint8_t type, uint16_t packet_id, size_t *out_len) {
    *out_len = 4U;
    uint8_t flags = (type == MQTT_PUBREL) ? 0x02U : 0x00U;
    uint8_t *buf = (uint8_t *)malloc(*out_len);
    if (!buf) {
        return NULL;
    }
    buf[0] = (uint8_t)((type << 4U) | flags);
    buf[1] = 2U;
    uint8_t *p = buf + 2U;
    write_u16(&p, packet_id);
    return buf;
}

uint8_t *mqtt_build_pingresp(size_t *out_len) {
    *out_len = 2U;
    uint8_t *buf = (uint8_t *)malloc(*out_len);
    if (!buf) {
        return NULL;
    }
    buf[0] = (uint8_t)(MQTT_PINGRESP << 4U);
    buf[1] = 0U;
    return buf;
}

uint8_t *mqtt_build_publish(const char *topic, const uint8_t *payload, size_t payload_len,
                            uint8_t qos, bool retain, uint16_t packet_id, size_t *out_len) {
    if (!topic || qos > MQTT_QOS2) {
        return NULL;
    }
    size_t topic_len = strlen(topic);
    if (topic_len > UINT16_MAX || (qos == 0U && packet_id != 0U)) {
        return NULL;
    }
    size_t remaining = 2U + topic_len + payload_len + ((qos > 0U) ? 2U : 0U);
    uint8_t rlb[4];
    int rllen = mqtt_encode_remaining_length(rlb, remaining);
    if (rllen <= 0) {
        return NULL;
    }
    *out_len = 1U + (size_t)rllen + remaining;
    uint8_t *buf = (uint8_t *)malloc(*out_len);
    if (!buf) {
        return NULL;
    }
    uint8_t *p = buf;
    uint8_t fixed_flags = (uint8_t)((qos << 1U) | (retain ? 0x01U : 0x00U));
    *p++ = (uint8_t)((MQTT_PUBLISH << 4U) | fixed_flags);
    if (append_remaining(&p, remaining) < 0) {
        free(buf);
        return NULL;
    }
    write_u16(&p, (uint16_t)topic_len);
    memcpy(p, topic, topic_len);
    p += topic_len;
    if (qos > 0U) {
        write_u16(&p, packet_id);
    }
    if (payload_len > 0U) {
        memcpy(p, payload, payload_len);
    }
    return buf;
}

bool mqtt_topic_name_is_valid(const char *topic) {
    return topic && topic[0] != '\0' && strchr(topic, '+') == NULL && strchr(topic, '#') == NULL;
}

bool mqtt_topic_filter_is_valid(const char *filter) {
    if (!filter || filter[0] == '\0') {
        return false;
    }
    size_t n = strlen(filter);
    for (size_t i = 0U; i < n; ++i) {
        if (filter[i] == '#') {
            bool start_ok = (i == 0U || filter[i - 1U] == '/');
            bool end_ok = (i == n - 1U);
            if (!start_ok || !end_ok) {
                return false;
            }
        } else if (filter[i] == '+') {
            bool start_ok = (i == 0U || filter[i - 1U] == '/');
            bool end_ok = (i == n - 1U || filter[i + 1U] == '/');
            if (!start_ok || !end_ok) {
                return false;
            }
        }
    }
    return true;
}

bool mqtt_topic_matches(const char *filter, const char *topic) {
    if (!filter || !topic) {
        return false;
    }
    const char *f = filter;
    const char *t = topic;
    while (*f != '\0') {
        if (*f == '#') {
            return f[1] == '\0';
        }
        if (*f == '/' && f[1] == '#' && f[2] == '\0' && *t == '\0') {
            return true;
        }
        if (*f == '+') {
            while (*t != '\0' && *t != '/') {
                t++;
            }
            f++;
            if (*f == '\0') {
                return *t == '\0';
            }
            if (*f == '/' && *t == '/') {
                f++;
                t++;
                continue;
            }
            return false;
        }
        if (*f != *t) {
            return false;
        }
        f++;
        t++;
    }
    return *t == '\0';
}
