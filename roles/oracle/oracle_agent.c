/*
 * oracle_agent.c — NML Collective Oracle role.
 *
 * Knowledge layer: observes all agents, aggregates execution results using
 * z-score outlier detection and per-agent confidence weights, publishes
 * assessments to nml/assess/<phash>, and periodically generates program
 * specifications (with optional LLM) for the Architect.
 *
 * Usage:
 *   ./oracle_agent [--name NAME] [--broker HOST] [--broker-port PORT]
 *                  [--port HTTP_PORT] [--quorum N]
 *                  [--llm-host HOST] [--llm-port PORT] [--llm-path PATH]
 *
 * Inputs:  nml/result/#, nml/heartbeat/#, nml/announce/#
 * Outputs: nml/assess/<phash>, nml/spec, nml/data/vote
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/vote.h"
#include "../../edge/mqtt_transport.h"

/* Direct MQTT publish for custom topics (nml/assess/<phash>, nml/spec) */
#include "../../edge/mqtt/mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_ORACLE_SESSIONS   64
#define ORACLE_MAX_VOTES      64
#define MAX_AGENT_STATS       128
#define HTTP_BUF_SZ           8192
#define OUTLIER_ZSCORE        2.0f
#define CONFIDENCE_EWMA_ALPHA 0.2f   /* weight given to newest observation */
#define ASSESS_COOLDOWN_S     5      /* min seconds between re-assess per phash */
#define SESSION_MAX_AGE_S     300    /* expire oracle sessions after 5 minutes */
#define HEARTBEAT_INTERVAL_S  5
#define STALE_PEER_S          30
#define SPEC_INTERVAL_S       60     /* publish nml/spec once per minute */

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    char  voter[64];
    float score;
    int   is_outlier;
} OracleVote;

typedef struct {
    char       phash[17];                    /* 16 hex + NUL */
    OracleVote votes[ORACLE_MAX_VOTES];
    int        count;
    float      raw_mean;
    float      weighted_mean;
    float      confidence;  /* fraction of non-outlier voters, 0.0–1.0 */
    int        assessed;    /* 1 once published to nml/assess/<phash> */
    time_t     first_seen;
    time_t     last_assessed;
} OracleSession;

typedef struct {
    OracleSession sessions[MAX_ORACLE_SESSIONS];
    int           count;
} OracleTable;

typedef struct {
    char  name[64];
    float confidence;   /* EWMA: 1.0 = always matches consensus */
    int   vote_count;
    int   outlier_count;
} AgentStat;

typedef struct {
    AgentStat entries[MAX_AGENT_STATS];
    int       count;
} AgentStatTable;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static MQTTTransport  g_mqtt;
static PeerTable      g_peers;
static VoteTable      g_votes;
static OracleTable    g_oracle;
static AgentStatTable g_stats;

static const char *g_agent_name  = "oracle";
static char g_broker_host[128]   = "127.0.0.1";
static uint16_t g_broker_port    = 1883;
static uint16_t g_http_port      = 9002;
static int g_quorum              = 1;
static char g_llm_host[128]      = {0};
static uint16_t g_llm_port       = 8080;
static char g_llm_path[256]      = "/v1/chat/completions";

/* ── AgentStat helpers ───────────────────────────────────────────────── */

static AgentStat *stat_get_or_create(const char *name)
{
    for (int i = 0; i < g_stats.count; i++) {
        if (strcmp(g_stats.entries[i].name, name) == 0)
            return &g_stats.entries[i];
    }
    if (g_stats.count >= MAX_AGENT_STATS) return NULL;
    AgentStat *s = &g_stats.entries[g_stats.count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->confidence = 1.0f;   /* new agents start fully trusted */
    return s;
}

static float agent_confidence(const char *name)
{
    for (int i = 0; i < g_stats.count; i++) {
        if (strcmp(g_stats.entries[i].name, name) == 0)
            return g_stats.entries[i].confidence;
    }
    return 1.0f;    /* unknown agents default to fully trusted */
}

/* ── OracleSession helpers ───────────────────────────────────────────── */

static OracleSession *oracle_get_or_create(const char *phash)
{
    for (int i = 0; i < g_oracle.count; i++) {
        if (strcmp(g_oracle.sessions[i].phash, phash) == 0)
            return &g_oracle.sessions[i];
    }
    /* Evict the oldest session if table is full */
    if (g_oracle.count >= MAX_ORACLE_SESSIONS) {
        int oldest = 0;
        for (int i = 1; i < g_oracle.count; i++) {
            if (g_oracle.sessions[i].first_seen <
                g_oracle.sessions[oldest].first_seen)
                oldest = i;
        }
        /* Overwrite oldest by compacting the slot */
        g_oracle.sessions[oldest] = g_oracle.sessions[--g_oracle.count];
    }
    OracleSession *s = &g_oracle.sessions[g_oracle.count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->phash, phash, sizeof(s->phash) - 1);
    s->first_seen = time(NULL);
    s->confidence = 1.0f;
    return s;
}

static void oracle_add_vote(OracleSession *s, const char *voter, float score)
{
    /* Deduplicate by voter name */
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->votes[i].voter, voter) == 0) return;
    }
    if (s->count >= ORACLE_MAX_VOTES) return;
    OracleVote *v = &s->votes[s->count++];
    strncpy(v->voter, voter, sizeof(v->voter) - 1);
    v->score      = score;
    v->is_outlier = 0;
}

/*
 * Run z-score outlier detection on session votes, compute weighted mean, and
 * update per-agent confidence weights via EWMA.
 */
static void oracle_assess(OracleSession *s)
{
    if (s->count == 0) return;

    /* 1. Unweighted mean */
    float sum = 0.0f;
    for (int i = 0; i < s->count; i++) sum += s->votes[i].score;
    float raw_mean = sum / (float)s->count;

    /* 2. Population standard deviation */
    float var = 0.0f;
    for (int i = 0; i < s->count; i++) {
        float d = s->votes[i].score - raw_mean;
        var += d * d;
    }
    float stddev = (s->count > 1) ? sqrtf(var / (float)s->count) : 0.0f;

    /* 3. Flag outliers: z-score > OUTLIER_ZSCORE */
    int outlier_count = 0;
    for (int i = 0; i < s->count; i++) {
        float z = (stddev > 0.0f)
                  ? fabsf(s->votes[i].score - raw_mean) / stddev
                  : 0.0f;
        s->votes[i].is_outlier = (z > OUTLIER_ZSCORE) ? 1 : 0;
        if (s->votes[i].is_outlier) outlier_count++;
    }

    /* 4. Weighted mean over non-outlier votes, weighted by agent confidence */
    float w_sum = 0.0f, w_count = 0.0f;
    for (int i = 0; i < s->count; i++) {
        if (s->votes[i].is_outlier) continue;
        float w  = agent_confidence(s->votes[i].voter);
        w_sum   += w * s->votes[i].score;
        w_count += w;
    }
    float weighted_mean = (w_count > 0.0f) ? (w_sum / w_count) : raw_mean;

    /* 5. Confidence: fraction of inlier voters */
    float confidence = (s->count > 0)
                       ? 1.0f - (float)outlier_count / (float)s->count
                       : 0.0f;

    s->raw_mean      = raw_mean;
    s->weighted_mean = weighted_mean;
    s->confidence    = confidence;

    /* 6. Update per-agent confidence via EWMA */
    for (int i = 0; i < s->count; i++) {
        AgentStat *stat = stat_get_or_create(s->votes[i].voter);
        if (!stat) continue;
        stat->vote_count++;
        if (s->votes[i].is_outlier) {
            stat->outlier_count++;
            /* Outlier: decay confidence */
            stat->confidence = (1.0f - CONFIDENCE_EWMA_ALPHA) * stat->confidence;
        } else {
            /* Inlier: reward based on agreement with weighted mean */
            float deviation  = fabsf(s->votes[i].score - weighted_mean);
            float agreement  = 1.0f / (1.0f + deviation);
            stat->confidence = (1.0f - CONFIDENCE_EWMA_ALPHA) * stat->confidence
                             + CONFIDENCE_EWMA_ALPHA * agreement;
        }
        if (stat->confidence < 0.0f) stat->confidence = 0.0f;
        if (stat->confidence > 1.0f) stat->confidence = 1.0f;
    }
}

/* ── MQTT publish helpers ────────────────────────────────────────────── */

static void publish_assessment(const OracleSession *s)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "nml/assess/%s", s->phash);

    char json[2048];
    int n = snprintf(json, sizeof(json),
        "{\"phash\":\"%s\","
         "\"raw_mean\":%.4f,"
         "\"weighted_mean\":%.4f,"
         "\"confidence\":%.4f,"
         "\"vote_count\":%d,"
         "\"outliers\":[",
        s->phash, s->raw_mean, s->weighted_mean,
        s->confidence, s->count);

    int first = 1;
    for (int i = 0; i < s->count && n < (int)sizeof(json) - 4; i++) {
        if (!s->votes[i].is_outlier) continue;
        if (!first) n += snprintf(json + n, sizeof(json) - (size_t)n, ",");
        n += snprintf(json + n, sizeof(json) - (size_t)n,
                      "\"%s\"", s->votes[i].voter);
        first = 0;
    }
    n += snprintf(json + n, sizeof(json) - (size_t)n, "]}");

    mqtt_publish(&g_mqtt.client, topic,
                 (const uint8_t *)json, (size_t)n,
                 MQTT_PUBLISH_QOS_1);
    mqtt_sync(&g_mqtt.client);

    printf("[oracle] assessed %s  weighted_mean=%.4f  confidence=%.4f  votes=%d\n",
           s->phash, s->weighted_mean, s->confidence, s->count);
}

/*
 * Vote on a data pool item.  approve=1 to endorse, 0 to reject.
 * Publishes to nml/data/vote for consumers (Sentient) to process.
 */
static void publish_data_vote(const char *hash, int approve, const char *reason)
{
    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"hash\":\"%s\",\"approve\":%s,\"reason\":\"%s\",\"voter\":\"%s\"}",
        hash, approve ? "true" : "false", reason, g_agent_name);

    mqtt_publish(&g_mqtt.client, "nml/data/vote",
                 (const uint8_t *)json, (size_t)n,
                 MQTT_PUBLISH_QOS_1);
    mqtt_sync(&g_mqtt.client);
    printf("[oracle] data_vote hash=%s approve=%d reason=%s\n",
           hash, approve, reason);
}

/* ── Message handlers ────────────────────────────────────────────────── */

static void handle_result(const char *sender, const char *payload)
{
    char  phash[17];
    float score;
    if (sscanf(payload, "%16[^:]:%f", phash, &score) != 2) return;
    phash[16] = '\0';

    /* Mirror into VoteTable for quorum tracking */
    vote_add(&g_votes, phash, sender, score, g_quorum, time(NULL));

    /* Add to oracle session */
    OracleSession *s = oracle_get_or_create(phash);
    if (!s) return;
    oracle_add_vote(s, sender, score);

    /* Assess when we have reached quorum, with cooldown on re-assessment */
    if (s->count >= g_quorum) {
        time_t now = time(NULL);
        if (!s->assessed ||
            (now - s->last_assessed) >= ASSESS_COOLDOWN_S) {
            oracle_assess(s);
            publish_assessment(s);
            s->assessed      = 1;
            s->last_assessed = now;
        }
    }
}

/* ── LLM spec generation ─────────────────────────────────────────────── */

/*
 * POST a prompt to an OpenAI-compatible /v1/chat/completions endpoint.
 * Extracts the text content from the first choice.
 * Returns chars written to out_buf, or -1 on failure.
 */
static int llm_complete(const char *prompt, char *out_buf, size_t out_sz)
{
    if (g_llm_host[0] == '\0') return -1;

    char req_body[2048];
    int body_len = snprintf(req_body, sizeof(req_body),
        "{\"model\":\"local\","
         "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
         "\"max_tokens\":256}",
        prompt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(g_llm_port);
    addr.sin_addr.s_addr = inet_addr(g_llm_host);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }

    char http_req[4096];
    int req_len = snprintf(http_req, sizeof(http_req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        g_llm_path, g_llm_host, g_llm_port, body_len, req_body);
    write(fd, http_req, (size_t)req_len);

    char resp[8192];
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (rn <= 0) return -1;
    resp[rn] = '\0';

    /* Skip HTTP headers */
    char *body = strstr(resp, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    /* Extract first "content":"..." from response JSON */
    char *p = strstr(body, "\"content\":");
    if (!p) return -1;
    p += 10;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;

    int i = 0;
    while (*p && *p != '"' && i < (int)out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;   /* skip escape prefix */
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    return i;
}

/*
 * Generate a program specification and publish to nml/spec.
 * Uses LLM if configured, otherwise falls back to a template.
 */
static void maybe_publish_spec(void)
{
    /* Count assessed sessions */
    int   assessed       = 0;
    float avg_confidence = 0.0f;
    for (int i = 0; i < g_oracle.count; i++) {
        if (g_oracle.sessions[i].assessed) {
            assessed++;
            avg_confidence += g_oracle.sessions[i].confidence;
        }
    }
    if (assessed == 0) return;
    avg_confidence /= (float)assessed;

    char spec[1024];
    int  spec_len = 0;
    int  used_llm = 0;

    if (g_llm_host[0] != '\0') {
        char prompt[512];
        snprintf(prompt, sizeof(prompt),
            "The NML collective has %d active agents and %d assessed programs "
            "with average confidence %.2f. "
            "Suggest a concise NML program spec (1-2 sentences) that probes "
            "collective health.",
            g_peers.count, assessed, avg_confidence);
        char content[768];
        if (llm_complete(prompt, content, sizeof(content)) > 0) {
            spec_len = snprintf(spec, sizeof(spec),
                "{\"source\":\"llm\",\"assessed\":%d,"
                 "\"avg_confidence\":%.4f,\"spec\":\"%s\"}",
                assessed, avg_confidence, content);
            used_llm = 1;
        }
    }

    if (!used_llm) {
        spec_len = snprintf(spec, sizeof(spec),
            "{\"source\":\"oracle\",\"assessed\":%d,"
             "\"avg_confidence\":%.4f,\"peers\":%d,"
             "\"spec\":\"Evaluate collective performance and report a score "
             "between 0.0 and 1.0.\"}",
            assessed, avg_confidence, g_peers.count);
    }

    mqtt_publish(&g_mqtt.client, "nml/spec",
                 (const uint8_t *)spec, (size_t)spec_len,
                 MQTT_PUBLISH_QOS_1);
    mqtt_sync(&g_mqtt.client);
    printf("[oracle] published spec (assessed=%d avg_conf=%.3f llm=%d)\n",
           assessed, avg_confidence, used_llm);
}

/* ── HTTP server ─────────────────────────────────────────────────────── */

static int g_http_fd = -1;

static int http_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static void http_send(int fd, int code, const char *body)
{
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Error", strlen(body));
    write(fd, hdr, (size_t)hdr_len);
    write(fd, body, strlen(body));
}

static void handle_http(int cfd)
{
    char req[1024];
    ssize_t n = read(cfd, req, sizeof(req) - 1);
    if (n <= 0) { close(cfd); return; }
    req[n] = '\0';

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        close(cfd); return;
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        char body[512];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"peers\":%d,\"sessions\":%d,\"agents\":%d}",
            g_agent_name, g_peers.count, g_oracle.count, g_stats.count);
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /assessments — list all assessed sessions */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/assessments") == 0) {
        char body[HTTP_BUF_SZ];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < g_oracle.count; i++) {
            const OracleSession *s = &g_oracle.sessions[i];
            if (!s->assessed) continue;
            if (pos > 1) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"phash\":\"%s\",\"weighted_mean\":%.4f,"
                 "\"confidence\":%.4f,\"votes\":%d}",
                s->phash, s->weighted_mean, s->confidence, s->count);
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /assessments/<phash> — detailed breakdown */
    if (strcmp(method, "GET") == 0 &&
        strncmp(path, "/assessments/", 13) == 0) {
        const char *phash = path + 13;
        for (int i = 0; i < g_oracle.count; i++) {
            const OracleSession *s = &g_oracle.sessions[i];
            if (strcmp(s->phash, phash) != 0) continue;
            char body[4096];
            int pos = snprintf(body, sizeof(body),
                "{\"phash\":\"%s\","
                 "\"raw_mean\":%.4f,"
                 "\"weighted_mean\":%.4f,"
                 "\"confidence\":%.4f,"
                 "\"votes\":[",
                s->phash, s->raw_mean, s->weighted_mean, s->confidence);
            for (int j = 0; j < s->count; j++) {
                if (j > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
                pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                    "{\"voter\":\"%s\",\"score\":%.4f,\"outlier\":%s}",
                    s->votes[j].voter, s->votes[j].score,
                    s->votes[j].is_outlier ? "true" : "false");
            }
            snprintf(body + pos, sizeof(body) - (size_t)pos, "]}");
            http_send(cfd, 200, body);
            close(cfd); return;
        }
        http_send(cfd, 404, "{\"error\":\"not found\"}");
        close(cfd); return;
    }

    /* GET /agents — per-agent confidence weights */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/agents") == 0) {
        char body[HTTP_BUF_SZ];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < g_stats.count; i++) {
            const AgentStat *st = &g_stats.entries[i];
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"name\":\"%s\",\"confidence\":%.4f,"
                 "\"votes\":%d,\"outliers\":%d}",
                st->name, st->confidence, st->vote_count, st->outlier_count);
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /*
     * POST /data/vote  body: {"hash":"<16hex>","approve":true,"reason":"..."}
     * Allows external callers to trigger a data-quality vote via Oracle.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/data/vote") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) { http_send(cfd, 400, "{\"error\":\"no body\"}"); close(cfd); return; }
        body_start += 4;

        /* Simple key extraction */
        char hash[17]   = {0};
        char reason[128] = "oracle-requested";
        int  approve     = 1;

        char *p = strstr(body_start, "\"hash\":");
        if (p) {
            p += 7; while (*p == ' ' || *p == '"') p++;
            int j = 0;
            while (*p && *p != '"' && j < 16) hash[j++] = *p++;
            hash[j] = '\0';
        }
        if (strstr(body_start, "\"approve\":false") ||
            strstr(body_start, "\"approve\": false")) approve = 0;
        char *rp = strstr(body_start, "\"reason\":");
        if (rp) {
            rp += 9; while (*rp == ' ' || *rp == '"') rp++;
            int j = 0;
            while (*rp && *rp != '"' && j < (int)sizeof(reason) - 1) reason[j++] = *rp++;
            reason[j] = '\0';
        }

        if (hash[0] == '\0') {
            http_send(cfd, 400, "{\"error\":\"hash required\"}");
            close(cfd); return;
        }
        publish_data_vote(hash, approve, reason);
        http_send(cfd, 200, "{\"ok\":true}");
        close(cfd); return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        http_send(cfd, 200, "{}");
        close(cfd); return;
    }

    http_send(cfd, 404, "{\"error\":\"not found\"}");
    close(cfd);
}

/* ── CLI usage ───────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--name NAME] [--broker HOST] [--broker-port PORT]\n"
        "          [--port HTTP_PORT] [--quorum N]\n"
        "          [--llm-host HOST] [--llm-port PORT] [--llm-path PATH]\n",
        prog);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* ── Parse CLI ── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            g_agent_name = argv[++i];
        else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc)
            strncpy(g_broker_host, argv[++i], sizeof(g_broker_host) - 1);
        else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc)
            g_broker_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_http_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--quorum") == 0 && i + 1 < argc)
            g_quorum = atoi(argv[++i]);
        else if (strcmp(argv[i], "--llm-host") == 0 && i + 1 < argc)
            strncpy(g_llm_host, argv[++i], sizeof(g_llm_host) - 1);
        else if (strcmp(argv[i], "--llm-port") == 0 && i + 1 < argc)
            g_llm_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--llm-path") == 0 && i + 1 < argc)
            strncpy(g_llm_path, argv[++i], sizeof(g_llm_path) - 1);
        else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── Identity ── */
    identity_init(g_agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);
    printf("[oracle] identity  machine=%s  node=%s\n",
           g_machine_hash_hex, g_node_id_hex);

    /* ── Init tables ── */
    peer_table_init(&g_peers);
    vote_table_init(&g_votes);
    memset(&g_oracle, 0, sizeof(g_oracle));
    memset(&g_stats,  0, sizeof(g_stats));

    /* ── MQTT ── */
    if (mqtt_transport_init(&g_mqtt, g_broker_host, g_broker_port,
                             g_agent_name, g_http_port,
                             g_identity_payload) != 0) {
        fprintf(stderr, "[oracle] failed to connect to broker %s:%u\n",
                g_broker_host, g_broker_port);
        return 1;
    }

    /* ── HTTP server ── */
    g_http_fd = http_listen(g_http_port);
    if (g_http_fd < 0) {
        fprintf(stderr, "[oracle] failed to bind HTTP on port %u\n", g_http_port);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }
    printf("[oracle] HTTP API on port %u\n", g_http_port);
    printf("[oracle] quorum=%d  broker=%s:%u%s\n",
           g_quorum, g_broker_host, g_broker_port,
           g_llm_host[0] ? "  llm=enabled" : "");

    /* ── Main loop ── */
    time_t last_heartbeat = 0;
    time_t last_spec      = 0;

    while (g_running) {
        time_t now = time(NULL);

        /* Wait up to 1 second for an HTTP connection */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_http_fd, &rfds);
        struct timeval tv = {1, 0};
        if (select(g_http_fd + 1, &rfds, NULL, NULL, &tv) > 0 &&
            FD_ISSET(g_http_fd, &rfds)) {
            int cfd = accept(g_http_fd, NULL, NULL);
            if (cfd >= 0) handle_http(cfd);
        }

        /* Sync MQTT I/O */
        mqtt_transport_sync(&g_mqtt, 0);

        /* Dispatch incoming NML messages */
        uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
        char    sender_ip[46];
        int     pkt_len;
        while ((pkt_len = mqtt_transport_recv(&g_mqtt, pkt,
                                              sizeof(pkt), sender_ip)) > 0) {
            int      type;
            char     pname[64];
            char     payload[NML_MAX_PROGRAM_LEN + 1];
            uint16_t pport;

            if (msg_parse(pkt, (size_t)pkt_len,
                          &type, pname, sizeof(pname),
                          &pport, payload, sizeof(payload)) != 0)
                continue;

            peer_upsert(&g_peers, pname,
                        sender_ip[0] ? sender_ip : NULL,
                        pport, NULL, now);

            if (type == MSG_RESULT)
                handle_result(pname, payload);
            /* ANNOUNCE and HEARTBEAT: peer_upsert above is sufficient */
        }

        /* Periodic heartbeat + sweep */
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL_S) {
            uint8_t hb[256];
            int hb_len = msg_encode(hb, sizeof(hb), MSG_HEARTBEAT,
                                    g_agent_name, g_http_port,
                                    g_identity_payload);
            if (hb_len > 0)
                mqtt_transport_publish(&g_mqtt, MSG_HEARTBEAT,
                                       hb, (size_t)hb_len);
            last_heartbeat = now;
            peer_sweep(&g_peers, now, STALE_PEER_S);
            vote_expire(&g_votes, now, SESSION_MAX_AGE_S);
        }

        /* Periodic spec publication */
        if (now - last_spec >= SPEC_INTERVAL_S) {
            maybe_publish_spec();
            last_spec = now;
        }
    }

    printf("[oracle] shutting down\n");
    mqtt_transport_close(&g_mqtt);
    if (g_http_fd >= 0) close(g_http_fd);
    return 0;
}
