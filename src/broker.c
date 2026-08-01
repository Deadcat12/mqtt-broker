#define _POSIX_C_SOURCE 200809L

#include "broker.h"
#include "mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_CLIENTS 1024
#define BACKLOG 128

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct subscription_node {
    char *filter;
    uint8_t qos;
    struct subscription_node *next;
} subscription_node;

typedef struct client {
    int fd;
    bool connected;
    char *client_id;
    uint16_t next_packet_id;
    subscription_node *subscriptions;
} client;

typedef struct retained_message {
    char *topic;
    uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    struct retained_message *next;
} retained_message;

typedef struct broker_state {
    client *clients[MAX_CLIENTS];
    retained_message *retained;
} broker_state;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signum) {
    (void)signum;
    g_stop = 1;
}

static void free_subscriptions(subscription_node *sub) {
    while (sub) {
        subscription_node *next = sub->next;
        free(sub->filter);
        free(sub);
        sub = next;
    }
}

static void client_destroy(client *c) {
    if (!c) {
        return;
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c->client_id);
    free_subscriptions(c->subscriptions);
    free(c);
}

static int set_reuseaddr(int fd) {
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

static void set_recv_timeout(int fd) {
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int create_listener(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int listen_fd = -1;
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd == -1) {
            continue;
        }
        (void)set_reuseaddr(listen_fd);
        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(listen_fd, BACKLOG) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(result);
    return listen_fd;
}

static ssize_t recv_all(int fd, uint8_t *buf, size_t len) {
    size_t done = 0U;
    while (done < len) {
        ssize_t n = recv(fd, buf + done, len - done, 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static int send_all(int fd, const uint8_t *buf, size_t len) {
    size_t done = 0U;
    while (done < len) {
        ssize_t n = send(fd, buf + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int send_packet(client *c, uint8_t *buf, size_t len) {
    if (!buf || !c) {
        free(buf);
        return -1;
    }
    int rc = send_all(c->fd, buf, len);
    free(buf);
    return rc;
}

static uint8_t *recv_mqtt_packet(int fd, size_t *out_len) {
    uint8_t first = 0U;
    if (recv_all(fd, &first, 1U) <= 0) {
        return NULL;
    }
    uint8_t rl_bytes[4];
    size_t rl_used = 0U;
    size_t remaining = 0U;
    size_t multiplier = 1U;
    do {
        if (rl_used >= 4U) {
            return NULL;
        }
        if (recv_all(fd, &rl_bytes[rl_used], 1U) <= 0) {
            return NULL;
        }
        remaining += (size_t)(rl_bytes[rl_used] & 127U) * multiplier;
        multiplier *= 128U;
        rl_used++;
    } while ((rl_bytes[rl_used - 1U] & 128U) != 0U);

    if (remaining > MQTT_MAX_PACKET_SIZE) {
        fprintf(stderr, "packet rejected: remaining length %zu > limit %u\n", remaining, MQTT_MAX_PACKET_SIZE);
        return NULL;
    }

    *out_len = 1U + rl_used + remaining;
    uint8_t *buf = (uint8_t *)malloc(*out_len);
    if (!buf) {
        return NULL;
    }
    buf[0] = first;
    memcpy(buf + 1U, rl_bytes, rl_used);
    if (remaining > 0U && recv_all(fd, buf + 1U + rl_used, remaining) <= 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static client *client_create(int fd) {
    client *c = (client *)calloc(1U, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->fd = fd;
    c->next_packet_id = 1U;
    return c;
}

static int add_client(broker_state *state, client *c) {
    for (size_t i = 0U; i < MAX_CLIENTS; ++i) {
        if (!state->clients[i]) {
            state->clients[i] = c;
            return 0;
        }
    }
    return -1;
}

static void remove_client_index(broker_state *state, size_t idx) {
    if (idx >= MAX_CLIENTS || !state->clients[idx]) {
        return;
    }
    fprintf(stderr, "client disconnected: %s\n", state->clients[idx]->client_id ? state->clients[idx]->client_id : "<not connected>");
    client_destroy(state->clients[idx]);
    state->clients[idx] = NULL;
}

static client *find_client_by_id(broker_state *state, const char *client_id, size_t *idx) {
    for (size_t i = 0U; i < MAX_CLIENTS; ++i) {
        client *c = state->clients[i];
        if (c && c->client_id && strcmp(c->client_id, client_id) == 0) {
            if (idx) {
                *idx = i;
            }
            return c;
        }
    }
    return NULL;
}

static uint16_t next_packet_id(client *c) {
    uint16_t id = c->next_packet_id++;
    if (c->next_packet_id == 0U) {
        c->next_packet_id = 1U;
    }
    return id;
}

static void subscription_add_or_update(client *c, const char *filter, uint8_t qos) {
    for (subscription_node *s = c->subscriptions; s; s = s->next) {
        if (strcmp(s->filter, filter) == 0) {
            s->qos = qos;
            return;
        }
    }
    subscription_node *s = (subscription_node *)calloc(1U, sizeof(*s));
    if (!s) {
        return;
    }
    s->filter = strdup(filter);
    s->qos = qos;
    s->next = c->subscriptions;
    c->subscriptions = s;
}

static void subscription_remove(client *c, const char *filter) {
    subscription_node **pp = &c->subscriptions;
    while (*pp) {
        subscription_node *cur = *pp;
        if (strcmp(cur->filter, filter) == 0) {
            *pp = cur->next;
            free(cur->filter);
            free(cur);
            return;
        }
        pp = &cur->next;
    }
}

static int best_matching_qos(client *c, const char *topic, uint8_t *qos) {
    bool found = false;
    uint8_t best = 0U;
    for (subscription_node *s = c->subscriptions; s; s = s->next) {
        if (mqtt_topic_matches(s->filter, topic)) {
            if (!found || s->qos > best) {
                best = s->qos;
            }
            found = true;
        }
    }
    if (found) {
        *qos = best;
        return 1;
    }
    return 0;
}

static void retained_store(broker_state *state, const char *topic, const uint8_t *payload, size_t payload_len, uint8_t qos) {
    retained_message **pp = &state->retained;
    while (*pp) {
        retained_message *cur = *pp;
        if (strcmp(cur->topic, topic) == 0) {
            if (payload_len == 0U) {
                *pp = cur->next;
                free(cur->topic);
                free(cur->payload);
                free(cur);
                return;
            }
            uint8_t *copy = (uint8_t *)malloc(payload_len);
            if (!copy) {
                return;
            }
            memcpy(copy, payload, payload_len);
            free(cur->payload);
            cur->payload = copy;
            cur->payload_len = payload_len;
            cur->qos = qos;
            return;
        }
        pp = &cur->next;
    }
    if (payload_len == 0U) {
        return;
    }
    retained_message *msg = (retained_message *)calloc(1U, sizeof(*msg));
    if (!msg) {
        return;
    }
    msg->topic = strdup(topic);
    msg->payload = (uint8_t *)malloc(payload_len);
    if (!msg->topic || !msg->payload) {
        free(msg->topic);
        free(msg->payload);
        free(msg);
        return;
    }
    memcpy(msg->payload, payload, payload_len);
    msg->payload_len = payload_len;
    msg->qos = qos;
    msg->next = state->retained;
    state->retained = msg;
}

static void retained_send_matching(broker_state *state, client *c, const char *filter, uint8_t sub_qos) {
    for (retained_message *msg = state->retained; msg; msg = msg->next) {
        if (mqtt_topic_matches(filter, msg->topic)) {
            uint8_t qos = msg->qos < sub_qos ? msg->qos : sub_qos;
            size_t out_len = 0U;
            uint16_t pid = qos > 0U ? next_packet_id(c) : 0U;
            uint8_t *out = mqtt_build_publish(msg->topic, msg->payload, msg->payload_len, qos, true, pid, &out_len);
            (void)send_packet(c, out, out_len);
        }
    }
}

static void publish_forward(broker_state *state, const mqtt_publish_packet *pub) {
    for (size_t i = 0U; i < MAX_CLIENTS; ++i) {
        client *target = state->clients[i];
        if (!target || !target->connected) {
            continue;
        }
        uint8_t sub_qos = 0U;
        if (best_matching_qos(target, pub->topic, &sub_qos) != 1) {
            continue;
        }
        uint8_t qos = pub->qos < sub_qos ? pub->qos : sub_qos;
        /* This broker completes incoming QoS2 handshakes but sends downstream QoS2 as QoS1. */
        if (qos == MQTT_QOS2) {
            qos = MQTT_QOS1;
        }
        size_t out_len = 0U;
        uint16_t pid = qos > 0U ? next_packet_id(target) : 0U;
        uint8_t *out = mqtt_build_publish(pub->topic, pub->payload, pub->payload_len, qos, false, pid, &out_len);
        if (send_packet(target, out, out_len) != 0) {
            fprintf(stderr, "warning: failed to forward PUBLISH to %s\n", target->client_id ? target->client_id : "<unknown>");
        }
    }
}

static int handle_connect(broker_state *state, client *c, const mqtt_connect_packet *conn) {
    size_t old_idx = 0U;
    client *old = find_client_by_id(state, conn->client_id, &old_idx);
    if (old && old != c) {
        remove_client_index(state, old_idx);
    }
    free(c->client_id);
    c->client_id = strdup(conn->client_id);
    if (!c->client_id) {
        return -1;
    }
    c->connected = true;
    size_t out_len = 0U;
    uint8_t *out = mqtt_build_connack(0U, 0U, &out_len);
    fprintf(stderr, "client connected: id=%s clean_session=%d keep_alive=%u\n",
            c->client_id, conn->clean_session ? 1 : 0, conn->keep_alive);
    return send_packet(c, out, out_len);
}

static int handle_subscribe(broker_state *state, client *c, const mqtt_subscribe_packet *sub) {
    if (!c->connected) {
        return -1;
    }
    uint8_t *return_codes = (uint8_t *)calloc(sub->count, 1U);
    if (!return_codes) {
        return -1;
    }
    for (size_t i = 0U; i < sub->count; ++i) {
        uint8_t qos = sub->items[i].qos;
        return_codes[i] = qos;
        if (qos != 0x80U) {
            subscription_add_or_update(c, sub->items[i].topic, qos);
            fprintf(stderr, "subscribe: client=%s filter=%s qos=%u\n", c->client_id, sub->items[i].topic, qos);
        }
    }
    size_t out_len = 0U;
    uint8_t *out = mqtt_build_suback(sub->packet_id, return_codes, sub->count, &out_len);
    int rc = send_packet(c, out, out_len);
    for (size_t i = 0U; i < sub->count; ++i) {
        if (sub->items[i].qos != 0x80U) {
            retained_send_matching(state, c, sub->items[i].topic, sub->items[i].qos);
        }
    }
    free(return_codes);
    return rc;
}

static int handle_unsubscribe(client *c, const mqtt_unsubscribe_packet *unsub) {
    if (!c->connected) {
        return -1;
    }
    for (size_t i = 0U; i < unsub->count; ++i) {
        subscription_remove(c, unsub->topics[i]);
        fprintf(stderr, "unsubscribe: client=%s filter=%s\n", c->client_id, unsub->topics[i]);
    }
    size_t out_len = 0U;
    uint8_t *out = mqtt_build_unsuback(unsub->packet_id, &out_len);
    return send_packet(c, out, out_len);
}

static int handle_publish(broker_state *state, client *c, const mqtt_publish_packet *pub) {
    if (!c->connected) {
        return -1;
    }
    fprintf(stderr, "publish: client=%s topic=%s qos=%u retain=%d bytes=%zu\n",
            c->client_id, pub->topic, pub->qos, pub->retain ? 1 : 0, pub->payload_len);
    if (pub->retain) {
        retained_store(state, pub->topic, pub->payload, pub->payload_len, pub->qos);
    }
    publish_forward(state, pub);
    if (pub->qos == MQTT_QOS1) {
        size_t out_len = 0U;
        uint8_t *out = mqtt_build_ack(MQTT_PUBACK, pub->packet_id, &out_len);
        return send_packet(c, out, out_len);
    }
    if (pub->qos == MQTT_QOS2) {
        size_t out_len = 0U;
        uint8_t *out = mqtt_build_ack(MQTT_PUBREC, pub->packet_id, &out_len);
        return send_packet(c, out, out_len);
    }
    return 0;
}

static int handle_packet(broker_state *state, client *c, mqtt_packet *packet) {
    switch (packet->type) {
        case MQTT_CONNECT:
            return handle_connect(state, c, &packet->body.connect);
        case MQTT_SUBSCRIBE:
            return handle_subscribe(state, c, &packet->body.subscribe);
        case MQTT_UNSUBSCRIBE:
            return handle_unsubscribe(c, &packet->body.unsubscribe);
        case MQTT_PUBLISH:
            return handle_publish(state, c, &packet->body.publish);
        case MQTT_PINGREQ: {
            size_t out_len = 0U;
            uint8_t *out = mqtt_build_pingresp(&out_len);
            return send_packet(c, out, out_len);
        }
        case MQTT_PUBREC: {
            size_t out_len = 0U;
            uint8_t *out = mqtt_build_ack(MQTT_PUBREL, packet->body.ack.packet_id, &out_len);
            return send_packet(c, out, out_len);
        }
        case MQTT_PUBREL: {
            size_t out_len = 0U;
            uint8_t *out = mqtt_build_ack(MQTT_PUBCOMP, packet->body.ack.packet_id, &out_len);
            return send_packet(c, out, out_len);
        }
        case MQTT_PUBACK:
        case MQTT_PUBCOMP:
            return 0;
        case MQTT_DISCONNECT:
            return 1;
        default:
            return -1;
    }
}

static void free_retained(retained_message *msg) {
    while (msg) {
        retained_message *next = msg->next;
        free(msg->topic);
        free(msg->payload);
        free(msg);
        msg = next;
    }
}

int broker_start(const char *host, const char *port) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int listen_fd = create_listener(host, port);
    if (listen_fd < 0) {
        perror("cannot create listening socket");
        return 1;
    }
    broker_state state;
    memset(&state, 0, sizeof(state));
    fprintf(stderr, "sol-broker listening on %s:%s\n", host ? host : "0.0.0.0", port);

    while (!g_stop) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int maxfd = listen_fd;
        for (size_t i = 0U; i < MAX_CLIENTS; ++i) {
            if (state.clients[i]) {
                FD_SET(state.clients[i]->fd, &readfds);
                if (state.clients[i]->fd > maxfd) {
                    maxfd = state.clients[i]->fd;
                }
            }
        }
        int rc = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        if (FD_ISSET(listen_fd, &readfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                set_recv_timeout(fd);
                client *c = client_create(fd);
                if (!c) {
                    fprintf(stderr, "failed to create client; closing accepted socket\n");
                    close(fd);
                } else if (add_client(&state, c) != 0) {
                    fprintf(stderr, "too many clients; closing accepted socket\n");
                    client_destroy(c);
                }
            }
        }
        for (size_t i = 0U; i < MAX_CLIENTS; ++i) {
            client *c = state.clients[i];
            if (!c || !FD_ISSET(c->fd, &readfds)) {
                continue;
            }
            size_t packet_len = 0U;
            uint8_t *raw = recv_mqtt_packet(c->fd, &packet_len);
            if (!raw) {
                remove_client_index(&state, i);
                continue;
            }
            mqtt_packet packet;
            char err[256];
            err[0] = '\0';
            if (mqtt_parse_packet(raw, packet_len, &packet, err, sizeof(err)) != 0) {
                fprintf(stderr, "malformed packet from %s: %s\n", c->client_id ? c->client_id : "<not connected>", err);
                free(raw);
                remove_client_index(&state, i);
                continue;
            }
            free(raw);
            int hrc = handle_packet(&state, c, &packet);
            mqtt_packet_free(&packet);
            if (hrc != 0) {
                remove_client_index(&state, i);
            }
        }
    }

    for (size_t i = 0U; i < MAX_CLIENTS; ++i) {
        client_destroy(state.clients[i]);
    }
    free_retained(state.retained);
    close(listen_fd);
    fprintf(stderr, "sol-broker stopped\n");
    return 0;
}
