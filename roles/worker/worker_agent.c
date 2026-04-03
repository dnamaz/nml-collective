/*
 * worker_agent.c — NML Collective Worker role.
 *
 * Receives signed NML programs, executes them against local data, and
 * publishes results back to the mesh.
 *
 * Transport (selected at startup):
 *   MQTT — primary; connect to Herald broker via --broker HOST
 *   UDP  — fallback; multicast on LAN when --broker is omitted
 *
 * Usage:
 *   ./worker_agent [--name NAME] [--port PORT] [--data FILE]
 *                  [--data-name NAME] [--require-signed]
 *                  [--broker HOST] [--broker-port PORT]
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/udp.h"
#include "../../edge/crypto.h"
#include "../../edge/nml_exec.h"
#include "../../edge/report.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/vote.h"
#include "../../edge/http_client.h"
#include "../../edge/mqtt_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat.h"

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static PeerTable     g_peers;
static VoteTable     g_votes;

/* Transport — one or the other is active */
static int           g_use_mqtt   = 0;
static MQTTTransport g_mqtt;
static UDPContext    g_udp;

/* Agent identity (promoted to globals for send_result / broadcast_msg) */
static const char   *g_agent_name = EDGE_AGENT_NAME;
static uint16_t      g_agent_port = EDGE_HTTP_PORT;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/*
 * Parse the NebulaDisk binary header (magic NML\x02) and return a malloc'd
 * copy of the content section (NUL-terminated).  Returns NULL on error.
 */
static char *nebula_extract_content(const uint8_t *buf, size_t buf_len)
{
    if (buf_len < 4 || memcmp(buf, "NML\x02", 4) != 0)
        return NULL;

    size_t pos = 4 + 8;
    if (pos + 2 > buf_len) return NULL;
    pos++;
    uint8_t author_len = buf[pos++];
    pos += author_len + 8;
    if (pos + 1 > buf_len) return NULL;
    uint8_t ndims = buf[pos++];
    pos += (size_t)ndims * 4;
    if (pos + 1 + 4 > buf_len) return NULL;
    pos++;
    uint32_t content_len = ((uint32_t)buf[pos]   << 24) |
                           ((uint32_t)buf[pos+1] << 16) |
                           ((uint32_t)buf[pos+2] <<  8) |
                            (uint32_t)buf[pos+3];
    pos += 4;
    if (pos + content_len > buf_len) return NULL;

    char *out = malloc(content_len + 1);
    if (!out) return NULL;
    memcpy(out, buf + pos, content_len);
    out[content_len] = '\0';
    return out;
}

/*
 * Fetch data from a sentient peer (HTTP GET /data/get?name=X → /objects/hash).
 */
static char *fetch_data(const char *sentient_ip, uint16_t sentient_port,
                        const char *data_name,
                        char *resp_buf, size_t resp_buf_sz)
{
    char path[256];
    snprintf(path, sizeof(path), "/data/get?name=%s", data_name);
    int n = http_get(sentient_ip, sentient_port, path, resp_buf, resp_buf_sz);
    if (n <= 0) {
        fprintf(stderr, "[%s] fetch: GET %s failed\n", g_agent_name, path);
        return NULL;
    }

    char *p = strstr(resp_buf, "\"hash\"");
    if (!p) return NULL;
    p = strchr(p + 6, '"');
    if (!p) return NULL;
    p++;
    char hash[65] = {0};
    size_t hi = 0;
    while (*p && *p != '"' && hi < sizeof(hash) - 1)
        hash[hi++] = *p++;
    if (!hi) return NULL;

    snprintf(path, sizeof(path), "/objects/%s", hash);
    n = http_get(sentient_ip, sentient_port, path, resp_buf, resp_buf_sz);
    if (n <= 0) return NULL;

    char *content = nebula_extract_content((const uint8_t *)resp_buf, (size_t)n);
    if (content) {
        printf("[%s] fetched data '%s' (hash=%s)\n", g_agent_name, data_name, hash);
    } else {
        content = malloc((size_t)n + 1);
        if (content) { memcpy(content, resp_buf, (size_t)n); content[n] = '\0'; }
        printf("[%s] fetched raw data '%s' (%d bytes)\n", g_agent_name, data_name, n);
    }
    return content;
}

static const PeerEntry *find_sentient(void)
{
    for (int i = 0; i < g_peers.count; i++) {
        const PeerEntry *e = &g_peers.entries[i];
        if (e->active && !e->quarantined &&
            e->ip[0] != '\0' &&
            strncmp(e->name, "sentient", 8) == 0)
            return e;
    }
    return NULL;
}

/* ── Transport-agnostic send helpers ─────────────────────────────────── */

static void send_result(const char *phash, const char *score_str)
{
    if (g_use_mqtt) {
        char result_payload[64];
        snprintf(result_payload, sizeof(result_payload), "%s:%s", phash, score_str);
        uint8_t buf[256];
        int n = msg_encode(buf, sizeof(buf), MSG_RESULT,
                           g_agent_name, g_agent_port, result_payload);
        if (n > 0)
            mqtt_transport_publish(&g_mqtt, MSG_RESULT, buf, (size_t)n);
    } else {
        report_send_udp(&g_udp, g_agent_name, g_agent_port, phash, score_str);
    }
}

static void send_msg(int type, const char *payload)
{
    uint8_t buf[256];
    int n = msg_encode(buf, sizeof(buf), type, g_agent_name, g_agent_port, payload);
    if (n <= 0) return;
    if (g_use_mqtt)
        mqtt_transport_publish(&g_mqtt, type, buf, (size_t)n);
    else
        udp_send(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                 buf, (size_t)n);
}

/* ── Message handlers ────────────────────────────────────────────────── */

static void handle_program(const char *compact_payload,
                            const char *peer_name,
                            char **local_data_ptr,
                            const char *data_name,
                            char *fetch_buf, size_t fetch_buf_sz,
                            int require_signed)
{
    static char program[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact_payload, program, sizeof(program)) < 0) {
        fprintf(stderr, "[%s] compact decode overflow from %s\n",
                g_agent_name, peer_name);
        return;
    }

    const char *body = program;
    int is_signed = 0;

    if (strncmp(program, "SIGN ", 5) == 0) {
        char signer[64] = {0};
        int vrc = crypto_verify_program(program, strlen(program),
                                        signer, sizeof(signer), &body);
        if (vrc == -3) {
            fprintf(stderr, "[%s] REJECTED invalid signature from %s\n",
                    g_agent_name, peer_name);
            return;
        }
        if (vrc == 0) {
            printf("[%s] verified — signed by '%s'\n", g_agent_name, signer);
            is_signed = 1;
        }
    } else if (require_signed) {
        fprintf(stderr, "[%s] REJECTED unsigned program from %s (--require-signed)\n",
                g_agent_name, peer_name);
        return;
    }

    /* Fetch data on demand if not already loaded */
    if (!*local_data_ptr && data_name && *data_name) {
        const PeerEntry *sentient = find_sentient();
        if (sentient) {
            printf("[%s] fetching data '%s' from %s (%s:%u)\n",
                   g_agent_name, data_name, sentient->name,
                   sentient->ip, sentient->port);
            *local_data_ptr = fetch_data(sentient->ip, sentient->port,
                                         data_name, fetch_buf, fetch_buf_sz);
        } else {
            fprintf(stderr, "[%s] no sentient peer available to fetch data\n",
                    g_agent_name);
        }
    }

    if (!*local_data_ptr) {
        fprintf(stderr, "[%s] skipping program — no data available\n", g_agent_name);
        return;
    }

    char phash[17];
    crypto_program_hash(body, phash);
    printf("[%s] executing %s (signed=%d) from %s\n",
           g_agent_name, phash, is_signed, peer_name);

    char score_str[32];
    int rc = nml_exec_run(body, *local_data_ptr, score_str, sizeof(score_str));
    if (rc == 0) {
        printf("[%s] score=%s hash=%s\n", g_agent_name, score_str, phash);
        send_result(phash, score_str);
    } else if (rc == -2) {
        fprintf(stderr, "[%s] no score key in result (hash=%s)\n",
                g_agent_name, phash);
    } else {
        fprintf(stderr, "[%s] execution failed (hash=%s)\n", g_agent_name, phash);
    }
}

static void handle_result(const char *payload, const char *peer_name)
{
    char phash[17] = {0};
    float score = 0.0f;
    if (sscanf(payload, "%16[^:]:%f", phash, &score) == 2) {
        int r = vote_add(&g_votes, phash, peer_name, score, 1, time(NULL));
        if (r == 1) {
            float mean;
            vote_get_result(&g_votes, phash, &mean);
            printf("[%s] COMMIT hash=%s mean_score=%.6f\n",
                   g_agent_name, phash, mean);
        }
    } else {
        printf("[%s] result from %s: %s\n", g_agent_name, peer_name, payload);
    }
}

static void dispatch(int type, const char *peer_name, const char *sender_ip,
                     uint16_t peer_port, const char *payload,
                     char **local_data_ptr, const char *data_name,
                     char *fetch_buf, size_t fetch_buf_sz,
                     int require_signed, time_t now)
{
    if (strcmp(peer_name, g_agent_name) == 0) return;

    switch (type) {
    case MSG_ANNOUNCE:
        peer_upsert(&g_peers, peer_name, sender_ip, peer_port, payload, now);
        printf("[%s] peer: %s ip=%s (port %u)\n",
               g_agent_name, peer_name, sender_ip[0] ? sender_ip : "?", peer_port);
        break;

    case MSG_HEARTBEAT:
        peer_upsert(&g_peers, peer_name, sender_ip, peer_port, payload, now);
        break;

    case MSG_PROGRAM: {
        PeerEntry *sender = peer_get(&g_peers, peer_name);
        if (sender && sender->quarantined) {
            fprintf(stderr, "[%s] IGNORED program from quarantined peer %s\n",
                    g_agent_name, peer_name);
            break;
        }
        handle_program(payload, peer_name,
                       local_data_ptr, data_name,
                       fetch_buf, fetch_buf_sz, require_signed);
        break;
    }

    case MSG_RESULT:
        handle_result(payload, peer_name);
        break;

    case MSG_ENFORCE: {
        char type_str[8] = {0}, target[64] = {0}, reason[128] = {0};
        const char *p = payload;
        size_t tl = strcspn(p, "|");
        if (tl < sizeof(type_str)) { memcpy(type_str, p, tl); p += tl; }
        if (*p == '|') p++;
        size_t nl = strcspn(p, "|");
        if (nl < sizeof(target)) { memcpy(target, p, nl); p += nl; }
        if (*p == '|') { p++; strncpy(reason, p, sizeof(reason) - 1); }

        if (strncmp(type_str, "Q", 1) == 0) {
            peer_quarantine(&g_peers, target, reason);
            printf("[%s] enforced quarantine: %s\n", g_agent_name, target);
        } else if (strncmp(type_str, "U", 1) == 0) {
            PeerEntry *pe = peer_get(&g_peers, target);
            if (pe) { pe->quarantined = 0; pe->active = 1; }
            printf("[%s] quarantine lifted: %s\n", g_agent_name, target);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    compat_winsock_init();

    const char *data_path    = NULL;
    const char *data_name    = NULL;
    int         require_signed = 0;
    char        broker_host[128] = "127.0.0.1";
    uint16_t    broker_port      = 1883;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            g_agent_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_agent_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_path = argv[++i];
        } else if (strcmp(argv[i], "--data-name") == 0 && i + 1 < argc) {
            data_name = argv[++i];
        } else if (strcmp(argv[i], "--require-signed") == 0) {
            require_signed = 1;
        } else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc) {
            strncpy(broker_host, argv[++i], sizeof(broker_host) - 1);
            g_use_mqtt = 1;
        } else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc) {
            broker_port = (uint16_t)atoi(argv[++i]);
            g_use_mqtt = 1;
        } else {
            fprintf(stderr,
                "Usage: %s [--name NAME] [--port PORT] [--data FILE]\n"
                "          [--data-name NAME] [--require-signed]\n"
                "          [--broker HOST] [--broker-port PORT]\n",
                argv[0]);
            return 1;
        }
    }

    char *local_data = NULL;
    if (data_path) {
        local_data = read_file(data_path);
        if (!local_data)
            fprintf(stderr, "[%s] WARNING: could not read data file %s\n",
                    g_agent_name, data_path);
        else
            printf("[%s] loaded data from %s\n", g_agent_name, data_path);
    }

    size_t fetch_buf_sz = NML_MAX_TENSOR_SIZE + 8192;
    char *fetch_buf = (data_name && !local_data) ? malloc(fetch_buf_sz) : NULL;

    identity_init(g_agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);
    printf("[%s] identity: %s\n", g_agent_name, g_identity_payload);

    peer_table_init(&g_peers);
    vote_table_init(&g_votes);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── Transport init ── */
    if (g_use_mqtt) {
        if (mqtt_transport_init(&g_mqtt, broker_host, broker_port,
                                 g_agent_name, g_agent_port,
                                 g_identity_payload) != 0) {
            fprintf(stderr, "[%s] failed to connect to broker %s:%u\n",
                    g_agent_name, broker_host, broker_port);
            free(local_data); free(fetch_buf);
            return 1;
        }
        printf("[%s] connected to broker %s:%u (port=%u require_signed=%d)\n",
               g_agent_name, broker_host, broker_port,
               g_agent_port, require_signed);
    } else {
        if (udp_init(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT) < 0) {
            fprintf(stderr, "[%s] failed to init UDP\n", g_agent_name);
            free(local_data); free(fetch_buf);
            return 1;
        }
        printf("[%s] joined %s:%u (port=%u require_signed=%d)\n",
               g_agent_name, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
               g_agent_port, require_signed);
        send_msg(MSG_ANNOUNCE, g_identity_payload);
    }

    if (data_name && !local_data)
        printf("[%s] will fetch data '%s' from sentient on first program\n",
               g_agent_name, data_name);

    static uint8_t in_buf[65536];
    static char    peer_name[64];
    static char    payload[NML_MAX_PROGRAM_LEN + 1];
    static char    sender_ip[46];

    time_t last_heartbeat = time(NULL);
    time_t last_sweep     = last_heartbeat;

    while (g_running) {
        time_t now = time(NULL);

        if (g_use_mqtt) {
            /* MQTT: sync I/O with 1-second timeout, drain message queue */
            mqtt_transport_sync(&g_mqtt, 1000);

            int pkt_len;
            while ((pkt_len = mqtt_transport_recv(&g_mqtt, in_buf,
                                                   sizeof(in_buf), sender_ip)) > 0) {
                int      type;
                uint16_t peer_port;
                if (msg_parse(in_buf, (size_t)pkt_len,
                              &type, peer_name, sizeof(peer_name),
                              &peer_port, payload, sizeof(payload)) < 0)
                    continue;
                dispatch(type, peer_name, sender_ip, peer_port, payload,
                         &local_data, data_name, fetch_buf, fetch_buf_sz,
                         require_signed, now);
            }
        } else {
            /* UDP: blocking recv with 1-second timeout */
            sender_ip[0] = '\0';
            int received = udp_recv(&g_udp, in_buf, sizeof(in_buf), 1000, sender_ip);
            if (received > 0) {
                int      type;
                uint16_t peer_port;
                if (msg_parse(in_buf, (size_t)received,
                              &type, peer_name, sizeof(peer_name),
                              &peer_port, payload, sizeof(payload)) >= 0) {
                    dispatch(type, peer_name, sender_ip, peer_port, payload,
                             &local_data, data_name, fetch_buf, fetch_buf_sz,
                             require_signed, now);
                }
            }
        }

        if (now - last_heartbeat >= HEARTBEAT_INTERVAL) {
            send_msg(MSG_HEARTBEAT, g_identity_payload);
            last_heartbeat = now;
        }

        if (now - last_sweep >= 30) {
            peer_sweep(&g_peers, now, HEARTBEAT_INTERVAL * 6);
            vote_expire(&g_votes, now, 120);
            last_sweep = now;
        }
    }

    printf("\n[%s] shutting down\n", g_agent_name);
    if (g_use_mqtt)
        mqtt_transport_close(&g_mqtt);
    else
        udp_close(&g_udp);
    free(local_data);
    free(fetch_buf);
    compat_winsock_cleanup();
    return 0;
}
