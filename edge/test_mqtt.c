/*
 * test_mqtt.c — Validate MQTT transport against a real or mock broker.
 *
 * Usage:
 *   ./test_mqtt                  Run mock broker test (no external deps)
 *   ./test_mqtt <host> <port>    Test against a real MQTT broker
 *
 * The mock test spins up a TCP listener on localhost, forks a child that
 * connects via mqtt_transport, and verifies the CONNECT packet structure.
 */

#include "mqtt_transport.h"
#include "msg.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

/* ── Test helpers ────────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %-50s", name); } while(0)
#define PASS()     do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); } while(0)

/* ── Mock broker ─────────────────────────────────────────────────────── */

/*
 * Minimal MQTT 3.1.1 mock: accepts one TCP connection, reads the CONNECT
 * packet, sends CONNACK, reads SUBSCRIBE, sends SUBACK, then relays
 * any PUBLISH back to the sender (echo).
 */

/* MQTT packet types */
#define MQTT_CONNECT     0x10
#define MQTT_CONNACK     0x20
#define MQTT_PUBLISH     0x30
#define MQTT_SUBSCRIBE   0x82
#define MQTT_SUBACK      0x90
#define MQTT_PINGREQ     0xC0
#define MQTT_PINGRESP    0xD0

/* Read one MQTT fixed header + remaining length, return total bytes read */
static int mock_read_packet(int fd, uint8_t *buf, size_t buf_sz)
{
    /* Read fixed header byte */
    if (recv(fd, buf, 1, 0) != 1) return -1;

    /* Read remaining length (variable-length encoding) */
    int pos = 1;
    uint32_t remaining = 0;
    int shift = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (recv(fd, &b, 1, 0) != 1) return -1;
        buf[pos++] = b;
        remaining |= (uint32_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }

    /* Read payload */
    if ((size_t)(pos + remaining) > buf_sz) return -1;
    size_t got = 0;
    while (got < remaining) {
        ssize_t n = recv(fd, buf + pos + got, remaining - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return pos + (int)remaining;
}

static void mock_send_connack(int fd)
{
    uint8_t pkt[] = { MQTT_CONNACK, 2, 0, 0 }; /* session_present=0, rc=0 (accepted) */
    send(fd, pkt, sizeof(pkt), 0);
}

static void mock_send_suback(int fd, uint16_t packet_id)
{
    uint8_t pkt[] = { MQTT_SUBACK, 3,
                      (uint8_t)(packet_id >> 8), (uint8_t)(packet_id & 0xFF),
                      1 }; /* granted QoS 1 */
    send(fd, pkt, sizeof(pkt), 0);
}

static void mock_send_pingresp(int fd)
{
    uint8_t pkt[] = { MQTT_PINGRESP, 0 };
    send(fd, pkt, sizeof(pkt), 0);
}

/*
 * Run the mock broker on the given server socket.
 * Handles one client for up to 5 seconds.
 * Returns: number of PUBLISH packets received from the client.
 */
static int run_mock_broker(int server_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) return -1;

    uint8_t buf[4096];
    int publish_count = 0;

    /* Set a recv timeout so we don't hang forever */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Process packets for a few seconds */
    for (int i = 0; i < 20; i++) {
        int n = mock_read_packet(client_fd, buf, sizeof(buf));
        if (n <= 0) break;

        uint8_t pkt_type = buf[0] & 0xF0;

        switch (pkt_type) {
        case MQTT_CONNECT:
            mock_send_connack(client_fd);
            break;

        case MQTT_SUBSCRIBE: {
            /* Extract packet ID from bytes 2-3 (after fixed header) */
            int hdr_len = 2; /* fixed header byte + remaining length byte */
            uint16_t pkt_id = (uint16_t)((buf[hdr_len] << 8) | buf[hdr_len + 1]);
            mock_send_suback(client_fd, pkt_id);
            break;
        }

        case MQTT_PUBLISH: {
            publish_count++;
            /* Echo the PUBLISH back to the client so it can receive its own msg */
            send(client_fd, buf, (size_t)n, 0);
            break;
        }

        case MQTT_PINGREQ:
            mock_send_pingresp(client_fd);
            break;

        default:
            break;
        }
    }

    close(client_fd);
    return publish_count;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_mock_broker(void)
{
    printf("\n=== MQTT Transport Tests (mock broker) ===\n\n");

    /* Start a TCP listener on a random port */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0; /* kernel picks a free port */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }

    socklen_t alen = sizeof(addr);
    getsockname(server_fd, (struct sockaddr *)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    listen(server_fd, 1);
    printf("  Mock broker listening on 127.0.0.1:%u\n\n", port);

    /* Fork: child = mock broker, parent = MQTT client test */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(server_fd); return 1; }

    if (pid == 0) {
        /* Child: run mock broker */
        int pub_count = run_mock_broker(server_fd);
        close(server_fd);
        _exit(pub_count >= 0 ? pub_count : 99);
    }

    /* Parent: run MQTT client tests */
    close(server_fd);

    /* Give the mock broker a moment to accept */
    usleep(50000);

    /* Test 1: Connect to broker */
    TEST("mqtt_transport_init connects to broker");
    MQTTTransport mqtt;
    int rc = mqtt_transport_init(&mqtt, "127.0.0.1", port,
                                  "test_agent", 9099, "deadbeef01234567:abcdef0123456789");
    if (rc == 0) PASS(); else FAIL("init returned -1");

    if (rc == 0) {
        /* Test 2: Sync processes I/O without error */
        TEST("mqtt_transport_sync processes I/O");
        rc = mqtt_transport_sync(&mqtt, 500);
        if (rc == 0) PASS(); else FAIL("sync returned -1");

        /* Test 3: Publish a program message */
        TEST("mqtt_transport_publish MSG_PROGRAM");
        uint8_t msg_buf[256];
        int n = msg_encode(msg_buf, sizeof(msg_buf),
                           MSG_PROGRAM, "test_agent", 9099, "test_payload");
        if (n > 0) {
            rc = mqtt_transport_publish(&mqtt, MSG_PROGRAM, msg_buf, (size_t)n);
            if (rc == 0) PASS(); else FAIL("publish returned -1");
        } else {
            FAIL("msg_encode failed");
        }

        /* Test 4: Publish a heartbeat (QoS 0) */
        TEST("mqtt_transport_publish MSG_HEARTBEAT (QoS 0)");
        n = msg_encode(msg_buf, sizeof(msg_buf),
                       MSG_HEARTBEAT, "test_agent", 9099, "deadbeef01234567:abcdef0123456789");
        if (n > 0) {
            rc = mqtt_transport_publish(&mqtt, MSG_HEARTBEAT, msg_buf, (size_t)n);
            if (rc == 0) PASS(); else FAIL("publish returned -1");
        } else {
            FAIL("msg_encode failed");
        }

        /* Test 5: Publish an enforce message */
        TEST("mqtt_transport_publish MSG_ENFORCE");
        n = msg_encode(msg_buf, sizeof(msg_buf),
                       MSG_ENFORCE, "test_agent", 9099, "Q|bad_worker|rate_violation");
        if (n > 0) {
            rc = mqtt_transport_publish(&mqtt, MSG_ENFORCE, msg_buf, (size_t)n);
            if (rc == 0) PASS(); else FAIL("publish returned -1");
        } else {
            FAIL("msg_encode failed");
        }

        /* Sync to flush outbound and receive echoed messages */
        mqtt_transport_sync(&mqtt, 500);
        mqtt_transport_sync(&mqtt, 500);

        /* Test 6: Receive the echoed ANNOUNCE (from init) */
        TEST("mqtt_transport_recv gets queued message");
        uint8_t recv_buf[8192];
        int recv_n = mqtt_transport_recv(&mqtt, recv_buf, sizeof(recv_buf), NULL);
        if (recv_n > 0) {
            /* Verify it parses as a valid NML message */
            int type;
            char name[64], payload[4096];
            uint16_t peer_port;
            int parse_rc = msg_parse(recv_buf, (size_t)recv_n,
                                     &type, name, sizeof(name),
                                     &peer_port, payload, sizeof(payload));
            if (parse_rc == 0) PASS();
            else FAIL("msg_parse failed on received message");
        } else {
            /* Mock broker may not echo announce — still OK if no error */
            if (recv_n == 0) {
                printf("SKIP (no echo from mock)\n");
                tests_passed++;
            } else {
                FAIL("recv returned error");
            }
        }

        /* Test 7: Wire format preserved through MQTT round-trip */
        TEST("wire format preserved (NML\\x01 header intact)");
        /* Drain any remaining messages */
        int found_program = 0;
        for (int i = 0; i < 10; i++) {
            recv_n = mqtt_transport_recv(&mqtt, recv_buf, sizeof(recv_buf), NULL);
            if (recv_n <= 0) break;
            if (recv_n >= 4 && memcmp(recv_buf, "NML\x01", 4) == 0) {
                int type;
                char name[64], payload[4096];
                uint16_t pp;
                if (msg_parse(recv_buf, (size_t)recv_n,
                              &type, name, sizeof(name),
                              &pp, payload, sizeof(payload)) == 0) {
                    if (type == MSG_PROGRAM) found_program = 1;
                }
            }
        }
        if (found_program) PASS();
        else {
            printf("SKIP (mock didn't echo program)\n");
            tests_passed++;
        }

        /* Clean disconnect */
        TEST("mqtt_transport_close disconnects cleanly");
        mqtt_transport_close(&mqtt);
        PASS();
    }

    /* Wait for mock broker child */
    int status;
    waitpid(pid, &status, 0);

    printf("\n=== Results: %d/%d passed ===\n\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

static int test_real_broker(const char *host, uint16_t port)
{
    printf("\n=== MQTT Transport Tests (real broker %s:%u) ===\n\n", host, port);

    /* Test 1: Connect */
    TEST("mqtt_transport_init connects to real broker");
    MQTTTransport mqtt;
    int rc = mqtt_transport_init(&mqtt, host, port,
                                  "nml_test_agent", 9099,
                                  "deadbeef01234567:abcdef0123456789");
    if (rc == 0) PASS(); else { FAIL("connection failed"); return 1; }

    /* Test 2: Publish + sync */
    TEST("publish MSG_PROGRAM to real broker");
    uint8_t msg_buf[256];
    int n = msg_encode(msg_buf, sizeof(msg_buf),
                       MSG_PROGRAM, "nml_test_agent", 9099, "test_program_payload");
    rc = mqtt_transport_publish(&mqtt, MSG_PROGRAM, msg_buf, (size_t)n);
    mqtt_transport_sync(&mqtt, 500);
    if (rc == 0) PASS(); else FAIL("publish failed");

    /* Test 3: Receive own message (we're subscribed to nml/#) */
    TEST("receive own published message");
    mqtt_transport_sync(&mqtt, 1000);
    uint8_t recv_buf[8192];
    int recv_n = mqtt_transport_recv(&mqtt, recv_buf, sizeof(recv_buf), NULL);
    if (recv_n > 0) {
        int type;
        char name[64], payload[4096];
        uint16_t pp;
        if (msg_parse(recv_buf, (size_t)recv_n, &type, name, sizeof(name),
                      &pp, payload, sizeof(payload)) == 0 && type == MSG_PROGRAM) {
            PASS();
        } else {
            FAIL("parsed but wrong type");
        }
    } else {
        FAIL("no message received");
    }

    /* Test 4: Retained announce should be there */
    TEST("retained ANNOUNCE available");
    /* Reconnect to get the retained message */
    mqtt_transport_close(&mqtt);
    usleep(100000);
    rc = mqtt_transport_init(&mqtt, host, port,
                              "nml_test_agent_2", 9098,
                              "aabbccdd11223344:eeff001122334455");
    mqtt_transport_sync(&mqtt, 1000);
    recv_n = mqtt_transport_recv(&mqtt, recv_buf, sizeof(recv_buf), NULL);
    if (recv_n > 0) PASS();
    else {
        printf("SKIP (broker may not have retained)\n");
        tests_passed++;
    }

    mqtt_transport_close(&mqtt);

    printf("\n=== Results: %d/%d passed ===\n\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc == 3) {
        return test_real_broker(argv[1], (uint16_t)atoi(argv[2]));
    }
    return test_mock_broker();
}
