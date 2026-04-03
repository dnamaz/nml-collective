/*
 * mqtt_transport.h — MQTT transport layer for NML collective agents.
 *
 * Drop-in alongside udp.c — agents choose UDP (LAN, 11μs) or MQTT (WAN,
 * reliable delivery, retained presence, credential management).
 *
 * The NML wire format (NML\x01 + type + name + port + payload) is preserved
 * inside MQTT PUBLISH payloads.  msg_parse / msg_encode work unchanged
 * regardless of which transport carried the message.
 *
 * MQTT topics:
 *   nml/announce/<name>   — presence (retained, Last Will clears on disconnect)
 *   nml/program           — signed programs (QoS 1)
 *   nml/result            — execution results (QoS 1)
 *   nml/heartbeat/<name>  — keepalive (QoS 0)
 *   nml/enforce           — quarantine / unquarantine (QoS 1)
 *
 * Backed by MQTT-C (vendored in edge/mqtt/, MIT license).
 */

#ifndef EDGE_MQTT_TRANSPORT_H
#define EDGE_MQTT_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "compat.h"
#include "mqtt/mqtt.h"
#include "msg.h"

/* ── Queue for received messages ────────────────────────────────────── */

#define MQTT_MSG_MAX_SZ    8192
#define MQTT_MSG_QUEUE_SZ  16
#define MQTT_IO_BUF_SZ     4096

typedef struct {
    uint8_t data[MQTT_MSG_MAX_SZ];
    int     len;
} MQTTQueuedMsg;

/* ── Transport context ──────────────────────────────────────────────── */

typedef struct {
    struct mqtt_client client;
    compat_socket_t    sockfd;
    uint8_t            sendbuf[MQTT_IO_BUF_SZ];
    uint8_t            recvbuf[MQTT_IO_BUF_SZ];
    char               agent_name[64];

    /* Ring buffer — incoming messages queued by the MQTT-C callback */
    MQTTQueuedMsg      queue[MQTT_MSG_QUEUE_SZ];
    int                queue_head;   /* next write position */
    int                queue_tail;   /* next read position  */
    int                queue_count;
} MQTTTransport;

/* ── API ────────────────────────────────────────────────────────────── */

/*
 * Connect to the MQTT broker and subscribe to all NML topics.
 *
 * Registers a Last Will: publish empty payload to nml/announce/<name>
 * with retain — clears the agent's retained presence on ungraceful
 * disconnect.
 *
 * Publishes an initial retained ANNOUNCE message so existing agents
 * discover this one immediately.
 *
 * Returns 0 on success, -1 on connection failure.
 */
int mqtt_transport_init(MQTTTransport *t,
                        const char *broker_host, uint16_t broker_port,
                        const char *agent_name, uint16_t agent_port,
                        const char *identity_payload);

/*
 * Disconnect cleanly and close the TCP socket.
 */
void mqtt_transport_close(MQTTTransport *t);

/*
 * Publish a pre-encoded NML message.
 *
 * buf / len contain the full NML\x01 wire-format message (from msg_encode).
 * msg_type is used to select the MQTT topic and QoS level:
 *   MSG_ANNOUNCE  → nml/announce/<name>  QoS 1, retained
 *   MSG_PROGRAM   → nml/program          QoS 1
 *   MSG_RESULT    → nml/result           QoS 1
 *   MSG_HEARTBEAT → nml/heartbeat/<name> QoS 0
 *   MSG_ENFORCE   → nml/enforce          QoS 1
 *
 * Returns 0 on success, -1 on error.
 */
int mqtt_transport_publish(MQTTTransport *t, int msg_type,
                           const uint8_t *buf, size_t len);

/*
 * Process MQTT I/O — call once per event-loop iteration.
 *
 * Reads from the TCP socket (with select timeout), processes incoming
 * PUBLISH messages via the MQTT-C callback (which queues them), and
 * sends any pending outbound data.
 *
 * Returns 0 on success, -1 on error (connection lost).
 */
int mqtt_transport_sync(MQTTTransport *t, int timeout_ms);

/*
 * Pop the next received message from the queue.
 *
 * Returns bytes copied into buf (> 0), 0 if queue is empty, -1 on error.
 * The returned data is a full NML\x01 wire-format message — pass it
 * directly to msg_parse().
 *
 * sender_ip_out is always set to the broker address (MQTT does not
 * expose per-message sender IPs).  May be NULL.
 */
int mqtt_transport_recv(MQTTTransport *t, uint8_t *buf, size_t buf_sz,
                        char *sender_ip_out);

#endif /* EDGE_MQTT_TRANSPORT_H */
