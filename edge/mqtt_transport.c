/*
 * mqtt_transport.c — MQTT transport layer for NML collective agents.
 *
 * See mqtt_transport.h for the API description.
 */

#include "mqtt_transport.h"
#include "compat.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ── MQTT-C publish callback ─────────────────────────────────────────── */

/*
 * Called by mqtt_sync() for every incoming PUBLISH message.
 * We copy the raw MQTT payload (which is an NML\x01 wire-format message)
 * into the ring buffer queue for later retrieval by mqtt_transport_recv().
 */
static void on_publish(void **state, struct mqtt_response_publish *published)
{
    MQTTTransport *t = *(MQTTTransport **)state;
    if (!t) return;

    size_t len = published->application_message_size;
    if (len == 0 || len > MQTT_MSG_MAX_SZ) return;

    /* Drop if queue is full */
    if (t->queue_count >= MQTT_MSG_QUEUE_SZ) return;

    MQTTQueuedMsg *slot = &t->queue[t->queue_head];
    memcpy(slot->data, published->application_message, len);
    slot->len = (int)len;

    size_t tlen = published->topic_name_size;
    if (tlen >= sizeof(slot->topic)) tlen = sizeof(slot->topic) - 1;
    memcpy(slot->topic, published->topic_name, tlen);
    slot->topic[tlen] = '\0';

    t->queue_head = (t->queue_head + 1) % MQTT_MSG_QUEUE_SZ;
    t->queue_count++;
}

/* ── TCP socket helpers ──────────────────────────────────────────────── */

static compat_socket_t tcp_connect(const char *host, uint16_t port)
{
    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == COMPAT_INVALID_SOCKET) return COMPAT_INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        compat_close_socket(fd);
        return COMPAT_INVALID_SOCKET;
    }

    /* Set non-blocking — MQTT-C expects this */
    compat_set_nonblocking(fd);

    return fd;
}

/* ── Topic helpers ───────────────────────────────────────────────────── */

static int topic_for_msg(int msg_type, const char *agent_name,
                         char *topic, size_t topic_sz)
{
    switch (msg_type) {
    case MSG_ANNOUNCE:
        snprintf(topic, topic_sz, "nml/announce/%s", agent_name);
        break;
    case MSG_PROGRAM:
        snprintf(topic, topic_sz, "nml/program");
        break;
    case MSG_RESULT:
        snprintf(topic, topic_sz, "nml/result");
        break;
    case MSG_HEARTBEAT:
        snprintf(topic, topic_sz, "nml/heartbeat/%s", agent_name);
        break;
    case MSG_ENFORCE:
        snprintf(topic, topic_sz, "nml/enforce");
        break;
    case MSG_SPEC:
        snprintf(topic, topic_sz, "nml/spec");
        break;
    default:
        return -1;
    }
    return 0;
}

static int qos_for_msg(int msg_type)
{
    switch (msg_type) {
    case MSG_HEARTBEAT: return 0;
    default:            return 1;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

int mqtt_transport_init(MQTTTransport *t,
                        const char *broker_host, uint16_t broker_port,
                        const char *agent_name, uint16_t agent_port,
                        const char *identity_payload)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->agent_name, sizeof(t->agent_name), "%s", agent_name);

    /* Connect TCP to broker */
    t->sockfd = tcp_connect(broker_host, broker_port);
    if (t->sockfd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[mqtt] failed to connect to %s:%u\n",
                broker_host, broker_port);
        return -1;
    }

    /* Initialize MQTT-C client */
    enum MQTTErrors err = mqtt_init(&t->client, t->sockfd,
                                    t->sendbuf, sizeof(t->sendbuf),
                                    t->recvbuf, sizeof(t->recvbuf),
                                    on_publish);
    if (err != MQTT_OK) {
        fprintf(stderr, "[mqtt] mqtt_init failed: %s\n", mqtt_error_str(err));
        compat_close_socket(t->sockfd);
        return -1;
    }

    /* Store pointer to ourselves for the callback */
    t->client.publish_response_callback_state = t;

    /* Build Last Will topic: nml/announce/<name> with empty payload, retained.
       This clears our retained presence on ungraceful disconnect. */
    char will_topic[128];
    snprintf(will_topic, sizeof(will_topic), "nml/announce/%s", agent_name);

    /* MQTT CONNECT — empty will message clears retained presence on disconnect */
    err = mqtt_connect(&t->client,
                       agent_name,      /* client_id */
                       will_topic,      /* will_topic */
                       "",              /* will_message (empty = clear retained) */
                       0,               /* will_message_size */
                       NULL,            /* user_name */
                       NULL,            /* password */
                       MQTT_CONNECT_WILL_RETAIN, /* connect_flags */
                       60);             /* keep_alive seconds */
    if (err != MQTT_OK) {
        fprintf(stderr, "[mqtt] mqtt_connect failed: %s\n", mqtt_error_str(err));
        compat_close_socket(t->sockfd);
        return -1;
    }

    /* Sync to send the CONNECT packet.
       On Windows, the first sync may fail if the broker is still
       initialising — retry a few times with a short delay. */
    int sync_ok = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (mqtt_sync(&t->client) == MQTT_OK) {
            sync_ok = 1;
            break;
        }
        /* Re-init and retry */
        compat_close_socket(t->sockfd);
        t->sockfd = tcp_connect(broker_host, broker_port);
        if (t->sockfd == COMPAT_INVALID_SOCKET) break;
        mqtt_init(&t->client, t->sockfd,
                  t->sendbuf, sizeof(t->sendbuf),
                  t->recvbuf, sizeof(t->recvbuf),
                  on_publish);
        t->client.publish_response_callback_state = t;
        mqtt_connect(&t->client, agent_name, will_topic,
                     "", 0, NULL, NULL, MQTT_CONNECT_WILL_RETAIN, 60);
    }
    if (!sync_ok) {
        fprintf(stderr, "[mqtt] initial sync failed\n");
        compat_close_socket(t->sockfd);
        return -1;
    }

    /* Subscribe to all NML topics */
    mqtt_subscribe(&t->client, "nml/#", 1);

    /* Publish initial retained ANNOUNCE */
    uint8_t announce_buf[256];
    int n = msg_encode(announce_buf, sizeof(announce_buf),
                       MSG_ANNOUNCE, agent_name, agent_port, identity_payload);
    if (n > 0) {
        mqtt_publish(&t->client, will_topic,
                     announce_buf, (size_t)n,
                     MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN);
    }

    /* Sync to send subscribe + announce */
    mqtt_sync(&t->client);

    printf("[mqtt] connected to %s:%u as '%s'\n",
           broker_host, broker_port, agent_name);
    return 0;
}

void mqtt_transport_close(MQTTTransport *t)
{
    if (t->sockfd != COMPAT_INVALID_SOCKET) {
        mqtt_disconnect(&t->client);
        mqtt_sync(&t->client);
        compat_close_socket(t->sockfd);
        t->sockfd = COMPAT_INVALID_SOCKET;
    }
}

int mqtt_transport_publish(MQTTTransport *t, int msg_type,
                           const uint8_t *buf, size_t len)
{
    char topic[128];
    if (topic_for_msg(msg_type, t->agent_name, topic, sizeof(topic)) < 0)
        return -1;

    int qos = qos_for_msg(msg_type);
    uint8_t flags = (uint8_t)(qos == 0 ? MQTT_PUBLISH_QOS_0 : MQTT_PUBLISH_QOS_1);

    /* Retain only ANNOUNCE messages */
    if (msg_type == MSG_ANNOUNCE)
        flags |= MQTT_PUBLISH_RETAIN;

    enum MQTTErrors err = mqtt_publish(&t->client, topic, buf, len, flags);
    if (err != MQTT_OK) {
        fprintf(stderr, "[mqtt] publish to %s failed: %s\n",
                topic, mqtt_error_str(err));
        return -1;
    }
    return 0;
}

int mqtt_transport_sync(MQTTTransport *t, int timeout_ms)
{
    /* Use select() to wait for data on the TCP socket */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(t->sockfd, &fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000L;

    struct timeval *tvp = timeout_ms < 0 ? NULL : &tv;
    select(COMPAT_SELECT_NFDS(t->sockfd), &fds, NULL, NULL, tvp);

    /* Process all pending I/O — sends outbound, receives inbound,
       fires on_publish callback for each incoming PUBLISH. */
    enum MQTTErrors err = mqtt_sync(&t->client);
    if (err != MQTT_OK) {
        fprintf(stderr, "[mqtt] sync error: %s\n", mqtt_error_str(err));
        return -1;
    }
    return 0;
}

int mqtt_transport_recv(MQTTTransport *t, uint8_t *buf, size_t buf_sz,
                        char *sender_ip_out)
{
    if (t->queue_count <= 0) return 0;

    MQTTQueuedMsg *slot = &t->queue[t->queue_tail];
    int len = slot->len;

    if (len <= 0 || (size_t)len > buf_sz) {
        /* Skip invalid or oversized message */
        t->queue_tail = (t->queue_tail + 1) % MQTT_MSG_QUEUE_SZ;
        t->queue_count--;
        return -1;
    }

    memcpy(buf, slot->data, (size_t)len);
    t->queue_tail = (t->queue_tail + 1) % MQTT_MSG_QUEUE_SZ;
    t->queue_count--;

    /* MQTT does not expose per-message sender IP — set to empty */
    if (sender_ip_out)
        sender_ip_out[0] = '\0';

    return len;
}

int mqtt_transport_recv_ex(MQTTTransport *t, uint8_t *buf, size_t buf_sz,
                           char *sender_ip_out, char *topic_out)
{
    if (t->queue_count <= 0) return 0;

    MQTTQueuedMsg *slot = &t->queue[t->queue_tail];
    int len = slot->len;

    if (len <= 0 || (size_t)len > buf_sz) {
        t->queue_tail = (t->queue_tail + 1) % MQTT_MSG_QUEUE_SZ;
        t->queue_count--;
        return -1;
    }

    memcpy(buf, slot->data, (size_t)len);
    if (topic_out)
        snprintf(topic_out, 128, "%s", slot->topic);

    t->queue_tail = (t->queue_tail + 1) % MQTT_MSG_QUEUE_SZ;
    t->queue_count--;

    if (sender_ip_out)
        sender_ip_out[0] = '\0';

    return len;
}
