/*
 * entry.c — Bare-metal entry point for NML Edge Worker on ARM Cortex-M4.
 *
 * Replaces main.c when building for MCU targets.  There is no argc/argv,
 * no filesystem, and no signal handling.  The agent name and port are
 * set at compile time via config.h (or overridden with -D flags).
 *
 * Call pattern:
 *
 *   // In your BSP / FreeRTOS task:
 *   arm_edge_init();          // sets up UDP, sends ANNOUNCE
 *   for (;;) {
 *       arm_edge_tick();      // processes one round of messages
 *       vTaskDelay(1);        // yield — optional
 *   }
 *
 * Or from a bare-metal super-loop:
 *
 *   arm_edge_init();
 *   while (1) arm_edge_tick();
 *
 * If you prefer a self-contained task function for FreeRTOS:
 *
 *   void arm_edge_task(void *arg) {
 *       (void)arg;
 *       arm_edge_init();
 *       for (;;) { arm_edge_tick(); vTaskDelay(10); }
 *   }
 *   xTaskCreate(arm_edge_task, "nml_edge", 4096, NULL, 2, NULL);
 */

#include "../../config.h"
#include "../../msg.h"
#include "../../udp.h"
#include "../../crypto.h"
#include "../../nml_exec.h"
#include "../../report.h"
#include "../../identity.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── State ───────────────────────────────────────────────────────────────── */

static UDPContext  _udp;
static const char *_agent_name = EDGE_AGENT_NAME;
static uint16_t    _agent_port = EDGE_HTTP_PORT;
static time_t      _last_hb    = 0;

/* Node identity — populated in arm_edge_init() before first ANNOUNCE */
static char    _machine_hash_hex[17];
static uint8_t _machine_hash_bytes[8];
static char    _node_id_hex[17];
static char    _identity_payload[34];

/*
 * Optional static data blob for the NML VM (replaces file loading).
 * Set this pointer before calling arm_edge_init() if you have
 * sensor readings or feature vectors to inject:
 *
 *   extern const char *arm_edge_data;
 *   arm_edge_data = "SLOT fraud_data SHAPE 1x6\nDATA 0.1,0.2,...\n";
 */
const char *arm_edge_data = NULL;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void handle_program(const char *compact_payload, const char *peer_name)
{
    static char program[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact_payload, program, sizeof(program)) < 0) {
        fprintf(stderr, "[%s] compact decode overflow from %s\n",
                _agent_name, peer_name);
        return;
    }

    const char *body = program;
    int is_signed = 0;

    if (strncmp(program, "SIGN ", 5) == 0) {
        char signer[64] = {0};
        int vrc = crypto_verify_program(program, strlen(program),
                                        signer, sizeof(signer), &body);
        if (vrc == -3) {
            fprintf(stderr, "[%s] REJECTED invalid sig from %s\n",
                    _agent_name, peer_name);
            return;
        }
        if (vrc == 0) {
            printf("[%s] verified — signed by '%s'\n", _agent_name, signer);
            is_signed = 1;
        }
    }

    char phash[17];
    crypto_program_hash(body, phash);

    printf("[%s] executing %s (signed=%d) from %s\n",
           _agent_name, phash, is_signed, peer_name);

    char score_str[32];
    int rc = nml_exec_run(body, arm_edge_data, score_str, sizeof(score_str));

    if (rc == 0) {
        printf("[%s] score=%s hash=%s\n", _agent_name, score_str, phash);
        report_send_udp(&_udp, _agent_name, _agent_port, phash, score_str);
    } else if (rc == -2) {
        fprintf(stderr, "[%s] no score key found (hash=%s)\n",
                _agent_name, phash);
    } else {
        fprintf(stderr, "[%s] execution error (hash=%s)\n",
                _agent_name, phash);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void arm_edge_init(void)
{
    identity_init(_agent_name,
                  _machine_hash_hex, _machine_hash_bytes,
                  _node_id_hex, _identity_payload);

    udp_init(&_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT);

    printf("[%s] joined %s:%u (port=%u) identity=%s\n",
           _agent_name, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
           _agent_port, _identity_payload);

    uint8_t buf[256];
    int n = msg_encode(buf, sizeof(buf),
                       MSG_ANNOUNCE, _agent_name, _agent_port, _identity_payload);
    if (n > 0) udp_send(&_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                         buf, (size_t)n);

    _last_hb = time(NULL);
}

void arm_edge_tick(void)
{
    static uint8_t in_buf[4096];   /* smaller RX buffer on MCU */
    static char    peer_name[64];
    static char    payload[NML_MAX_PROGRAM_LEN + 1];

    int received = udp_recv(&_udp, in_buf, sizeof(in_buf), 1000 /* ms */);

    if (received > 0) {
        int      type;
        uint16_t peer_port;

        if (msg_parse(in_buf, (size_t)received,
                      &type, peer_name, sizeof(peer_name),
                      &peer_port, payload, sizeof(payload)) < 0) {
            return;
        }

        if (strcmp(peer_name, _agent_name) == 0) return; /* self-filter */

        switch (type) {
        case MSG_ANNOUNCE:
            printf("[%s] peer: %s (port %u)\n",
                   _agent_name, peer_name, peer_port);
            break;

        case MSG_PROGRAM:
            handle_program(payload, peer_name);
            break;

        case MSG_RESULT:
            printf("[%s] result from %s: %s\n",
                   _agent_name, peer_name, payload);
            break;

        default:
            break;
        }
    }

    /* Heartbeat */
    time_t now = time(NULL);
    if (now - _last_hb >= HEARTBEAT_INTERVAL) {
        uint8_t hb[256];
        int n = msg_encode(hb, sizeof(hb),
                           MSG_HEARTBEAT, _agent_name, _agent_port, _identity_payload);
        if (n > 0) udp_send(&_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                             hb, (size_t)n);
        _last_hb = now;
    }
}
