/*
 * worker_agent.c — NML Collective Worker role (C implementation).
 *
 * Functionally identical to the standalone edge_worker binary but links
 * against libcollective.a, enabling future roles to reuse shared modules
 * (peer_table, vote, program_send, http_server).
 *
 * Usage:
 *   ./worker_agent [--name NAME] [--port PORT] [--data FILE] [--require-signed]
 *
 * Wire-compatible with the Python collective (same 4-message UDP protocol).
 */

#include "../config.h"
#include "../msg.h"
#include "../udp.h"
#include "../crypto.h"
#include "../nml_exec.h"
#include "../report.h"
#include "../identity.h"
#include "../peer_table.h"
#include "../vote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

/* Peer table — populated from ANNOUNCE/HEARTBEAT messages. */
static PeerTable g_peers;

/* Vote table — collects results for programs this agent submitted. */
static VoteTable g_votes;

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

/* ── Message handlers ────────────────────────────────────────────────── */

static void handle_program(UDPContext *udp,
                            const char *compact_payload,
                            const char *peer_name,
                            const char *agent_name, uint16_t agent_port,
                            const char *local_data,
                            int require_signed)
{
    static char program[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact_payload, program, sizeof(program)) < 0) {
        fprintf(stderr, "[%s] compact decode overflow from %s\n", agent_name, peer_name);
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
                    agent_name, peer_name);
            return;
        }
        if (vrc == 0) {
            printf("[%s] verified — signed by '%s'\n", agent_name, signer);
            is_signed = 1;
        }
    } else if (require_signed) {
        fprintf(stderr, "[%s] REJECTED unsigned program from %s (--require-signed)\n",
                agent_name, peer_name);
        return;
    }

    char phash[17];
    crypto_program_hash(body, phash);

    printf("[%s] executing %s (signed=%d) from %s\n",
           agent_name, phash, is_signed, peer_name);

    char score_str[32];
    int rc = nml_exec_run(body, local_data, score_str, sizeof(score_str));

    if (rc == 0) {
        printf("[%s] score=%s hash=%s\n", agent_name, score_str, phash);
        report_send_udp(udp, agent_name, agent_port, phash, score_str);
    } else if (rc == -2) {
        fprintf(stderr, "[%s] no score key in result (hash=%s)\n", agent_name, phash);
    } else {
        fprintf(stderr, "[%s] execution failed (hash=%s)\n", agent_name, phash);
    }
}

static void handle_result(const char *payload, const char *peer_name,
                           const char *agent_name)
{
    /* payload: "<hash16>:<score>" */
    char phash[17] = {0};
    float score = 0.0f;
    if (sscanf(payload, "%16[^:]:%f", phash, &score) == 2) {
        int quorum = 1; /* worker: accept first result as committed */
        int r = vote_add(&g_votes, phash, peer_name, score, quorum, time(NULL));
        if (r == 1) {
            float mean;
            vote_get_result(&g_votes, phash, &mean);
            printf("[%s] COMMIT hash=%s mean_score=%.6f\n", agent_name, phash, mean);
        }
    } else {
        printf("[%s] result from %s: %s\n", agent_name, peer_name, payload);
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *agent_name  = EDGE_AGENT_NAME;
    uint16_t    agent_port  = EDGE_HTTP_PORT;
    const char *data_path   = NULL;
    int         require_signed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            agent_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            agent_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_path = argv[++i];
        } else if (strcmp(argv[i], "--require-signed") == 0) {
            require_signed = 1;
        } else {
            fprintf(stderr, "Usage: %s [--name NAME] [--port PORT] "
                    "[--data FILE] [--require-signed]\n", argv[0]);
            return 1;
        }
    }

    char *local_data = NULL;
    if (data_path) {
        local_data = read_file(data_path);
        if (!local_data) {
            fprintf(stderr, "[%s] WARNING: could not read data file %s\n",
                    agent_name, data_path);
        } else {
            printf("[%s] loaded data from %s\n", agent_name, data_path);
        }
    }

    identity_init(agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);
    printf("[%s] identity: %s\n", agent_name, g_identity_payload);

    peer_table_init(&g_peers);
    vote_table_init(&g_votes);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    UDPContext udp;
    if (udp_init(&udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT) < 0) {
        fprintf(stderr, "[%s] failed to init UDP\n", agent_name);
        free(local_data);
        return 1;
    }

    printf("[%s] joined %s:%u (port=%u require_signed=%d)\n",
           agent_name, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
           agent_port, require_signed);

    uint8_t out_buf[256];
    int n = msg_encode(out_buf, sizeof(out_buf),
                       MSG_ANNOUNCE, agent_name, agent_port, g_identity_payload);
    if (n > 0) udp_send(&udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                         out_buf, (size_t)n);

    static uint8_t  in_buf[65536];
    static char     peer_name[64];
    static char     payload[NML_MAX_PROGRAM_LEN + 1];

    time_t last_heartbeat = time(NULL);
    time_t last_sweep     = last_heartbeat;

    while (g_running) {
        int received = udp_recv(&udp, in_buf, sizeof(in_buf), 1000);

        if (received > 0) {
            int      type;
            uint16_t peer_port;

            if (msg_parse(in_buf, (size_t)received,
                          &type, peer_name, sizeof(peer_name),
                          &peer_port, payload, sizeof(payload)) < 0) {
                continue;
            }

            if (strcmp(peer_name, agent_name) == 0) continue;

            switch (type) {
            case MSG_ANNOUNCE:
                peer_upsert(&g_peers, peer_name, peer_port, payload, time(NULL));
                printf("[%s] peer: %s (port %u)\n", agent_name, peer_name, peer_port);
                break;

            case MSG_HEARTBEAT:
                peer_upsert(&g_peers, peer_name, peer_port, payload, time(NULL));
                break;

            case MSG_PROGRAM:
                handle_program(&udp, payload, peer_name,
                               agent_name, agent_port,
                               local_data, require_signed);
                break;

            case MSG_RESULT:
                handle_result(payload, peer_name, agent_name);
                break;

            default:
                break;
            }
        }

        time_t now = time(NULL);

        if (now - last_heartbeat >= HEARTBEAT_INTERVAL) {
            n = msg_encode(out_buf, sizeof(out_buf),
                           MSG_HEARTBEAT, agent_name, agent_port, g_identity_payload);
            if (n > 0) udp_send(&udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                                 out_buf, (size_t)n);
            last_heartbeat = now;
        }

        /* Expire stale peers every 30 s */
        if (now - last_sweep >= 30) {
            peer_sweep(&g_peers, now, HEARTBEAT_INTERVAL * 6);
            vote_expire(&g_votes, now, 120);
            last_sweep = now;
        }
    }

    printf("\n[%s] shutting down\n", agent_name);
    udp_close(&udp);
    free(local_data);
    return 0;
}
