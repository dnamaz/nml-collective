/*
 * enforcer_agent.c — NML Collective Enforcer role.
 *
 * The collective's immune system.  Monitors every UDP message on the mesh,
 * verifies node identities, enforces rate limits, detects score outliers,
 * and quarantines misbehaving nodes.  Quarantine decisions are broadcast
 * to the mesh via MSG_ENFORCE so all agents respect them immediately.
 *
 * Three enforcement levels (matching nml_enforcer.py):
 *   WARNING    — logged; auto-expires after 1 h
 *   QUARANTINE — temporary isolation; gossiped via MSG_ENFORCE
 *   BLACKLIST  — permanent; requires sentient approval (via --approve flag)
 *
 * Enforcers cannot quarantine sentients.
 *
 * Port of: serve/nml_enforcer.py
 *
 * Transport (selected at startup):
 *   MQTT — primary; connect to Herald broker via --broker HOST
 *   UDP  — fallback; multicast on LAN when --broker is omitted
 *
 * Usage:
 *   ./enforcer_agent [--name NAME] [--port PORT]
 *                    [--broker HOST] [--broker-port PORT]
 *   ./enforcer_agent --approve AGENT_NAME   (blacklist approval, sentient only)
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/udp.h"
#include "../../edge/crypto.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/mqtt_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "compat.h"

/* ── Tunables ────────────────────────────────────────────────────────── */

#define WARN_THRESHOLD          3       /* warnings before quarantine considered */
#define QUARANTINE_THRESHOLD    5       /* evidence hits before auto-quarantine */
#define QUARANTINE_DURATION_SEC 3600    /* 1 hour default */
#define RATE_WINDOW_SEC         60
#define RATE_MAX_MSGS           20      /* max msgs from one agent per window */
#define RATE_RING_SZ            32
#define SCORE_OUTLIER_Z         2.5f    /* z-score threshold for score outliers */
#define SCORE_MIN_SAMPLES       5       /* min votes before outlier detection kicks in */
#define MONITOR_INTERVAL_SEC    15
#define MAX_THREAT_ENTRIES      64
#define MAX_EVIDENCE            16
#define MAX_IDENTITY_BINDINGS   64

/* ── Data structures ─────────────────────────────────────────────────── */

typedef struct {
    char   ev_type[32];   /* "identity_tampered", "rate_violation", etc. */
    char   detail[128];
    time_t ts;
} EvidenceEntry;

typedef struct {
    char name[64];
    int  active;

    /* Warnings */
    int  warn_count;
    time_t warn_times[16];

    /* Quarantine */
    int    quarantined;
    time_t quarantine_expires;    /* 0 = permanent */
    char   quarantine_reason[128];

    /* Blacklist */
    int  blacklisted;
    char blacklist_reason[128];

    /* Rate tracking: ring buffer of recent message timestamps */
    time_t rate_times[RATE_RING_SZ];
    int    rate_head;

    /* Score tracking (for z-score outlier detection via RESULT messages) */
    double score_sum;
    double score_sum_sq;
    int    score_n;

    /* Evidence log */
    EvidenceEntry evidence[MAX_EVIDENCE];
    int           evidence_n;
} ThreatEntry;

typedef struct {
    ThreatEntry entries[MAX_THREAT_ENTRIES];
    int         count;
} ThreatTable;

/* Machine ↔ agent identity bindings */
typedef struct {
    char machine_hash[17];
    char agent_name[64];
} IdentityBinding;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static PeerTable  g_peers;
static ThreatTable g_threats;

static IdentityBinding g_mach_to_agent[MAX_IDENTITY_BINDINGS];
static int             g_mach_count = 0;

/* Transport — one or the other is active */
static int           g_use_mqtt = 0;
static MQTTTransport g_mqtt;
static UDPContext    g_udp;

static const char   *g_agent_name;
static uint16_t      g_agent_port;

/* ── ThreatTable helpers ─────────────────────────────────────────────── */

static ThreatEntry *threat_get(const char *name)
{
    for (int i = 0; i < g_threats.count; i++) {
        if (g_threats.entries[i].active &&
            strncmp(g_threats.entries[i].name, name, 63) == 0)
            return &g_threats.entries[i];
    }
    return NULL;
}

static ThreatEntry *threat_get_or_create(const char *name)
{
    ThreatEntry *e = threat_get(name);
    if (e) return e;
    if (g_threats.count >= MAX_THREAT_ENTRIES) return NULL;
    e = &g_threats.entries[g_threats.count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->active = 1;
    return e;
}

static void threat_add_evidence(ThreatEntry *e, const char *ev_type,
                                 const char *detail)
{
    if (!e) return;
    int idx = e->evidence_n < MAX_EVIDENCE ? e->evidence_n
                                           : (e->evidence_n % MAX_EVIDENCE);
    strncpy(e->evidence[idx].ev_type, ev_type, sizeof(e->evidence[idx].ev_type) - 1);
    strncpy(e->evidence[idx].detail,  detail,  sizeof(e->evidence[idx].detail) - 1);
    e->evidence[idx].ts = time(NULL);
    if (e->evidence_n < MAX_EVIDENCE) e->evidence_n++;
}

/* Count evidence entries of a given type within the last window_sec seconds. */
static int threat_count_evidence(const ThreatEntry *e, const char *ev_type,
                                  time_t window_sec)
{
    if (!e) return 0;
    int n = 0;
    time_t cutoff = time(NULL) - window_sec;
    for (int i = 0; i < e->evidence_n; i++) {
        if (e->evidence[i].ts >= cutoff &&
            strncmp(e->evidence[i].ev_type, ev_type, 31) == 0)
            n++;
    }
    return n;
}

/* ── Rate limiting ───────────────────────────────────────────────────── */

/* Returns number of messages from this agent in the current window.
   Records the current timestamp. */
static int rate_track(ThreatEntry *e)
{
    if (!e) return 0;
    time_t now = time(NULL);
    e->rate_times[e->rate_head % RATE_RING_SZ] = now;
    e->rate_head++;

    int count = 0;
    int stored = e->rate_head < RATE_RING_SZ ? e->rate_head : RATE_RING_SZ;
    for (int i = 0; i < stored; i++) {
        if (now - e->rate_times[i] <= RATE_WINDOW_SEC)
            count++;
    }
    return count;
}

/* ── Identity registry ───────────────────────────────────────────────── */

static const char *ident_agent_for_machine(const char *machine_hash)
{
    for (int i = 0; i < g_mach_count; i++) {
        if (strncmp(g_mach_to_agent[i].machine_hash, machine_hash, 16) == 0)
            return g_mach_to_agent[i].agent_name;
    }
    return NULL;
}

static const char *ident_machine_for_agent(const char *agent_name)
{
    for (int i = 0; i < g_mach_count; i++) {
        if (strncmp(g_mach_to_agent[i].agent_name, agent_name, 63) == 0)
            return g_mach_to_agent[i].machine_hash;
    }
    return NULL;
}

static void ident_register(const char *machine_hash, const char *agent_name)
{
    /* Update existing */
    for (int i = 0; i < g_mach_count; i++) {
        if (strncmp(g_mach_to_agent[i].machine_hash, machine_hash, 16) == 0) {
            snprintf(g_mach_to_agent[i].agent_name,
                     sizeof(g_mach_to_agent[i].agent_name), "%s", agent_name);
            return;
        }
    }
    if (g_mach_count >= MAX_IDENTITY_BINDINGS) return;
    snprintf(g_mach_to_agent[g_mach_count].machine_hash,
             sizeof(g_mach_to_agent[g_mach_count].machine_hash), "%s", machine_hash);
    snprintf(g_mach_to_agent[g_mach_count].agent_name,
             sizeof(g_mach_to_agent[g_mach_count].agent_name),  "%s", agent_name);
    g_mach_count++;
}

/* ── Enforcement actions ─────────────────────────────────────────────── */

static void broadcast_enforce(const char *type_str, const char *target,
                               const char *reason)
{
    /* MSG_ENFORCE payload: "<type>|<target>|<reason>" */
    char payload[128];
    if (reason && *reason)
        snprintf(payload, sizeof(payload), "%s|%s|%s", type_str, target, reason);
    else
        snprintf(payload, sizeof(payload), "%s|%s", type_str, target);

    uint8_t buf[256];
    int n = msg_encode(buf, sizeof(buf), MSG_ENFORCE,
                       g_agent_name, g_agent_port, payload);
    if (n <= 0) return;
    if (g_use_mqtt)
        mqtt_transport_publish(&g_mqtt, MSG_ENFORCE, buf, (size_t)n);
    else
        udp_send(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                 buf, (size_t)n);
}

static void do_warn(const char *agent_name, const char *reason)
{
    ThreatEntry *e = threat_get_or_create(agent_name);
    if (!e) return;
    if (e->warn_count < 16) {
        e->warn_times[e->warn_count] = time(NULL);
    }
    e->warn_count++;
    printf("[%s] WARN %s: %s (total=%d)\n",
           g_agent_name, agent_name, reason, e->warn_count);
    broadcast_enforce("W", agent_name, reason);
}

static int is_sentient(const char *agent_name)
{
    return strncmp(agent_name, "sentient", 8) == 0;
}

static void do_quarantine(const char *agent_name, const char *reason,
                           time_t duration_sec)
{
    if (is_sentient(agent_name)) {
        printf("[%s] BLOCKED: cannot quarantine sentient '%s'\n",
               g_agent_name, agent_name);
        return;
    }

    ThreatEntry *e = threat_get_or_create(agent_name);
    if (!e) return;
    if (e->quarantined) return;  /* already quarantined */

    e->quarantined        = 1;
    e->quarantine_expires = duration_sec ? time(NULL) + duration_sec : 0;
    strncpy(e->quarantine_reason, reason, sizeof(e->quarantine_reason) - 1);

    peer_quarantine(&g_peers, agent_name, reason);

    printf("[%s] QUARANTINE %s: %s (duration=%lds)\n",
           g_agent_name, agent_name, reason, (long)duration_sec);
    broadcast_enforce("Q", agent_name, reason);
}

static void do_unquarantine(const char *agent_name)
{
    ThreatEntry *e = threat_get(agent_name);
    if (e) {
        e->quarantined        = 0;
        e->quarantine_expires = 0;
        e->quarantine_reason[0] = '\0';
    }
    PeerEntry *pe = peer_get(&g_peers, agent_name);
    if (pe) {
        pe->quarantined = 0;
        pe->active      = 1;
    }
    printf("[%s] UNQUARANTINE %s\n", g_agent_name, agent_name);
    broadcast_enforce("U", agent_name, "");
}

/* ── Identity check (called on every ANNOUNCE / HEARTBEAT) ───────────── */

static void check_identity(const char *agent_name, const char *payload)
{
    if (!payload || !*payload) return;  /* legacy node, skip */

    char mhash[17], nid[17];
    int rc = identity_verify_payload(agent_name, payload, mhash, nid);

    if (rc == -2) {
        char detail[128];
        snprintf(detail, sizeof(detail), "payload='%.40s'", payload);
        ThreatEntry *e = threat_get_or_create(agent_name);
        threat_add_evidence(e, "malformed_identity", detail);
        do_warn(agent_name, "Malformed identity payload");
        return;
    }

    if (rc == -3) {
        char detail[128];
        snprintf(detail, sizeof(detail), "payload='%.33s'", payload);
        ThreatEntry *e = threat_get_or_create(agent_name);
        threat_add_evidence(e, "identity_tampered", detail);
        do_quarantine(agent_name, "Tampered identity: node_id mismatch",
                      QUARANTINE_DURATION_SEC);
        return;
    }

    if (rc != 0) return;  /* -1 = empty payload (legacy), ignore */

    /* Rule 1: one-node-per-machine */
    const char *existing_agent = ident_agent_for_machine(mhash);
    if (existing_agent && strncmp(existing_agent, agent_name, 63) != 0) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "machine=%s already registered to '%s', new='%s'",
                 mhash, existing_agent, agent_name);
        ThreatEntry *e = threat_get_or_create(agent_name);
        threat_add_evidence(e, "machine_conflict", detail);
        do_quarantine(agent_name, detail, QUARANTINE_DURATION_SEC);
        return;
    }

    /* Rule 2: name-stability — same name must not appear from a different machine */
    const char *existing_machine = ident_machine_for_agent(agent_name);
    if (existing_machine && strncmp(existing_machine, mhash, 16) != 0) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "agent='%s' was on machine=%s, now claims %s",
                 agent_name, existing_machine, mhash);
        ThreatEntry *e = threat_get_or_create(agent_name);
        threat_add_evidence(e, "machine_spoofed", detail);
        do_quarantine(agent_name, detail, QUARANTINE_DURATION_SEC);
        return;
    }

    /* All good — record binding */
    if (!existing_agent) {
        printf("[%s] identity registered: %s machine=%s\n",
               g_agent_name, agent_name, mhash);
    }
    ident_register(mhash, agent_name);
}

/* ── Rate check (called on every message from a peer) ────────────────── */

static void check_rate(const char *agent_name, const char *msg_type_str)
{
    ThreatEntry *e = threat_get_or_create(agent_name);
    int rate = rate_track(e);
    if (rate > RATE_MAX_MSGS) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "%d msgs in %ds (type=%s)", rate, RATE_WINDOW_SEC, msg_type_str);
        threat_add_evidence(e, "rate_violation", detail);
        int violations = threat_count_evidence(e, "rate_violation", 3600);
        if (violations >= QUARANTINE_THRESHOLD) {
            do_quarantine(agent_name, detail, QUARANTINE_DURATION_SEC);
        } else {
            do_warn(agent_name, detail);
        }
    }
}

/* ── Score outlier detection (called on every MSG_RESULT) ────────────── */

static void check_score_outlier(const char *agent_name, float score,
                                  const char *phash)
{
    ThreatEntry *e = threat_get_or_create(agent_name);
    if (!e) return;

    e->score_sum    += score;
    e->score_sum_sq += (double)score * score;
    e->score_n++;

    if (e->score_n < SCORE_MIN_SAMPLES) return;

    double mean     = e->score_sum / e->score_n;
    double variance = (e->score_sum_sq / e->score_n) - (mean * mean);
    if (variance < 1e-9) return;  /* all scores identical, skip */

    double stddev = sqrt(variance);
    double z      = fabs(((double)score - mean) / stddev);

    if (z > SCORE_OUTLIER_Z) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "program=%s score=%.4f z=%.2f mean=%.4f",
                 phash, score, z, mean);
        threat_add_evidence(e, "score_outlier", detail);
        int outliers = threat_count_evidence(e, "score_outlier", 3600);
        if (outliers >= 3) {
            do_quarantine(agent_name,
                "Auto-quarantine: 3+ score outliers in 1h",
                QUARANTINE_DURATION_SEC);
        } else {
            do_warn(agent_name, detail);
        }
    }
}

/* ── MSG_ENFORCE handler (from another enforcer on the mesh) ─────────── */

static void handle_enforce_msg(const char *payload, const char *from_name)
{
    /* payload: "<type>|<target>[|<reason>]" */
    char type_str[8]  = {0};
    char target[64]   = {0};
    char reason[128]  = {0};

    const char *p = payload;
    size_t tlen = strcspn(p, "|");
    if (tlen >= sizeof(type_str)) return;
    memcpy(type_str, p, tlen);
    p += tlen;
    if (*p == '|') p++;

    size_t nlen = strcspn(p, "|");
    if (nlen >= sizeof(target)) return;
    memcpy(target, p, nlen);
    p += nlen;
    if (*p == '|') {
        p++;
        strncpy(reason, p, sizeof(reason) - 1);
    }

    /* Ignore self-referential messages */
    if (strncmp(target, g_agent_name, 63) == 0) return;

    if (strncmp(type_str, "Q", 1) == 0) {
        char full_reason[192];
        snprintf(full_reason, sizeof(full_reason),
                 "[via %s] %s", from_name, reason);
        ThreatEntry *e = threat_get_or_create(target);
        threat_add_evidence(e, "remote_quarantine", full_reason);
        if (e && !e->quarantined)
            do_quarantine(target, full_reason, QUARANTINE_DURATION_SEC);

    } else if (strncmp(type_str, "U", 1) == 0) {
        do_unquarantine(target);

    } else if (strncmp(type_str, "W", 1) == 0) {
        printf("[%s] WARN (via %s): %s %s\n",
               g_agent_name, from_name, target, reason);
    }
}

/* ── Periodic monitor ────────────────────────────────────────────────── */

static void monitor_pass(time_t now)
{
    /* Expire timed quarantines */
    for (int i = 0; i < g_threats.count; i++) {
        ThreatEntry *e = &g_threats.entries[i];
        if (!e->active || !e->quarantined) continue;
        if (e->quarantine_expires > 0 && now >= e->quarantine_expires) {
            printf("[%s] quarantine expired: %s\n", g_agent_name, e->name);
            do_unquarantine(e->name);
        }
    }

    /* Expire stale peers */
    peer_sweep(&g_peers, now, HEARTBEAT_INTERVAL * 6);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    compat_winsock_init();

    g_agent_name = EDGE_AGENT_NAME;
    g_agent_port = EDGE_HTTP_PORT;
    const char *approve_target = NULL;
    char     broker_host[128] = "127.0.0.1";
    uint16_t broker_port      = 1883;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            g_agent_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_agent_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--approve") == 0 && i + 1 < argc) {
            approve_target = argv[++i];
        } else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc) {
            strncpy(broker_host, argv[++i], sizeof(broker_host) - 1);
            g_use_mqtt = 1;
        } else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc) {
            broker_port = (uint16_t)atoi(argv[++i]);
            g_use_mqtt = 1;
        } else {
            fprintf(stderr, "Usage: %s [--name NAME] [--port PORT] "
                    "[--broker HOST] [--broker-port PORT] "
                    "[--approve AGENT]\n", argv[0]);
            return 1;
        }
    }

    identity_init(g_agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);
    printf("[%s] identity: %s\n", g_agent_name, g_identity_payload);

    peer_table_init(&g_peers);
    memset(&g_threats, 0, sizeof(g_threats));

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── Transport init ── */
    if (g_use_mqtt) {
        if (mqtt_transport_init(&g_mqtt, broker_host, broker_port,
                                 g_agent_name, g_agent_port,
                                 g_identity_payload) != 0) {
            fprintf(stderr, "[%s] failed to connect to broker %s:%u\n",
                    g_agent_name, broker_host, broker_port);
            return 1;
        }
        printf("[%s] connected to broker %s:%u — immune system active\n",
               g_agent_name, broker_host, broker_port);
    } else {
        if (udp_init(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT) < 0) {
            fprintf(stderr, "[%s] failed to init UDP\n", g_agent_name);
            return 1;
        }
        printf("[%s] joined %s:%u — immune system active\n",
               g_agent_name, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT);

        /* Announce presence on UDP */
        uint8_t ann_buf[256];
        int an = msg_encode(ann_buf, sizeof(ann_buf),
                            MSG_ANNOUNCE, g_agent_name, g_agent_port,
                            g_identity_payload);
        if (an > 0)
            udp_send(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                     ann_buf, (size_t)an);
    }

    /* One-shot: broadcast a blacklist approval */
    if (approve_target) {
        printf("[%s] broadcasting blacklist approval for '%s'\n",
               g_agent_name, approve_target);
        broadcast_enforce("B", approve_target, "sentient approved");
    }

    static uint8_t  in_buf[65536];
    static char     peer_name[64];
    static char     payload[NML_MAX_PROGRAM_LEN + 1];
    static char     sender_ip[46];

    time_t last_heartbeat = time(NULL);
    time_t last_monitor   = last_heartbeat;

    while (g_running) {
        time_t now = time(NULL);
        int received = 0;

        if (g_use_mqtt) {
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
                if (strcmp(peer_name, g_agent_name) == 0) continue;
                received = 1;

                int from_enforcer = (strncmp(peer_name, "enforcer", 8) == 0);
                switch (type) {
                case MSG_ANNOUNCE:
                    peer_upsert(&g_peers, peer_name, sender_ip, peer_port,
                                payload, now);
                    check_identity(peer_name, payload);
                    printf("[%s] peer: %s ip=%s port=%u\n",
                           g_agent_name, peer_name,
                           sender_ip[0] ? sender_ip : "?", peer_port);
                    break;
                case MSG_HEARTBEAT:
                    peer_upsert(&g_peers, peer_name, sender_ip, peer_port,
                                payload, now);
                    check_identity(peer_name, payload);
                    if (!from_enforcer)
                        check_rate(peer_name, "heartbeat");
                    break;
                case MSG_PROGRAM:
                    if (!from_enforcer)
                        check_rate(peer_name, "program");
                    break;
                case MSG_RESULT: {
                    if (from_enforcer) break;
                    check_rate(peer_name, "result");
                    char phash[17] = {0};
                    float score    = 0.0f;
                    if (sscanf(payload, "%16[^:]:%f", phash, &score) == 2)
                        check_score_outlier(peer_name, score, phash);
                    break;
                }
                case MSG_ENFORCE:
                    handle_enforce_msg(payload, peer_name);
                    break;
                default:
                    break;
                }
            }
        } else {
            sender_ip[0] = '\0';
            received = udp_recv(&g_udp, in_buf, sizeof(in_buf), 1000, sender_ip);

            if (received > 0) {
                int      type;
                uint16_t peer_port;

                if (msg_parse(in_buf, (size_t)received,
                              &type, peer_name, sizeof(peer_name),
                              &peer_port, payload, sizeof(payload)) < 0)
                    goto heartbeat;

                if (strcmp(peer_name, g_agent_name) == 0) goto heartbeat;

                int from_enforcer = (strncmp(peer_name, "enforcer", 8) == 0);

                switch (type) {
                case MSG_ANNOUNCE:
                    peer_upsert(&g_peers, peer_name, sender_ip, peer_port,
                                payload, now);
                    check_identity(peer_name, payload);
                    printf("[%s] peer: %s ip=%s port=%u\n",
                           g_agent_name, peer_name, sender_ip, peer_port);
                    break;
                case MSG_HEARTBEAT:
                    peer_upsert(&g_peers, peer_name, sender_ip, peer_port,
                                payload, now);
                    check_identity(peer_name, payload);
                    if (!from_enforcer)
                        check_rate(peer_name, "heartbeat");
                    break;
                case MSG_PROGRAM:
                    if (!from_enforcer)
                        check_rate(peer_name, "program");
                    break;
                case MSG_RESULT: {
                    if (from_enforcer) break;
                    check_rate(peer_name, "result");
                    char phash[17] = {0};
                    float score    = 0.0f;
                    if (sscanf(payload, "%16[^:]:%f", phash, &score) == 2)
                        check_score_outlier(peer_name, score, phash);
                    break;
                }
                case MSG_ENFORCE:
                    handle_enforce_msg(payload, peer_name);
                    break;
                default:
                    break;
                }
            }
        }

heartbeat:
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL) {
            uint8_t hb_buf[256];
            int hn = msg_encode(hb_buf, sizeof(hb_buf),
                                MSG_HEARTBEAT, g_agent_name, g_agent_port,
                                g_identity_payload);
            if (hn > 0) {
                if (g_use_mqtt)
                    mqtt_transport_publish(&g_mqtt, MSG_HEARTBEAT,
                                           hb_buf, (size_t)hn);
                else
                    udp_send(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                             hb_buf, (size_t)hn);
            }
            last_heartbeat = now;
        }

        if (now - last_monitor >= MONITOR_INTERVAL_SEC) {
            monitor_pass(now);
            last_monitor = now;
        }

        (void)received;
    }

    printf("\n[%s] shutting down\n", g_agent_name);
    if (g_use_mqtt)
        mqtt_transport_close(&g_mqtt);
    else
        udp_close(&g_udp);
    compat_winsock_cleanup();
    return 0;
}
