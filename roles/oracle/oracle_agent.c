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
#include "../../edge/http_util.h"

#include "compat.h"

/* Embedded landing page, generated from ui.html via `xxd -i`. */
#include "ui.html.h"

/* Call-site aliases into the shared edge helpers. */
#define http_send  http_send_json

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>

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
static uint16_t g_llm_port       = 443;
static char g_llm_path[256]      = "/api/v1/chat/completions";
static char g_llm_api_key[256]   = {0};
static char g_llm_model[128]     = "openai/gpt-4o-mini";
static char g_llm_provider[32]   = "openai";  /* "openai" or "anthropic" */

static char g_think_host[128]    = {0};
static uint16_t g_think_port     = 443;
static char g_think_path[256]    = "/api/v1/chat/completions";
static char g_think_model[128]   = {0};

/* Code model — local NML code generation (nml-v09-merged-6bit).
   When set, becomes stage 3; --llm-* becomes stage 1 context provider. */
static char g_code_host[128]     = {0};
static uint16_t g_code_port      = 8080;
static char g_code_path[256]     = "/v1/chat/completions";
static char g_code_model[128]    = {0};

static char g_log_file[512]      = {0};

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

/* ── Forward declarations ────────────────────────────────────────────── */

static void append_assessment_log(const OracleSession *s);

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
    append_assessment_log(s);
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

static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++] = '\\'; out[j++] = '"'; }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else                { out[j++] = c; }
    }
    out[j] = '\0';
}

/*
 * POST a prompt to an LLM endpoint via curl.
 * provider: "openai" (OpenAI-compatible, incl. OpenRouter + local mlx_lm)
 *           "anthropic" (api.anthropic.com /v1/messages)
 * api_key may be NULL for unauthenticated local models.
 * Returns chars written to out_buf, or -1 on failure.
 */
static int llm_curl(const char *host, uint16_t port, const char *path,
                    const char *api_key, const char *model,
                    const char *provider, const char *prompt,
                    int max_tokens, char *out_buf, size_t out_sz)
{
    if (!host || host[0] == '\0') return -1;
    int is_anthropic = provider && strcmp(provider, "anthropic") == 0;

    char esc[2048];
    json_escape(prompt, esc, sizeof(esc));

    char body[4096];
    snprintf(body, sizeof(body),
        "{\"model\":\"%s\","
         "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
         "\"max_tokens\":%d}",
        model, esc, max_tokens);

    /* Write body to temp file — avoids all shell quoting issues */
    char tmppath[64] = "/tmp/nml_llm_XXXXXX";
    int tmpfd = mkstemp(tmppath);
    if (tmpfd < 0) return -1;
    write(tmpfd, body, strlen(body));
    close(tmpfd);

    const char *scheme = (port == 443) ? "https" : "http";
    char port_part[16] = "";
    if (!((port == 443 && strcmp(scheme, "https") == 0) ||
          (port == 80  && strcmp(scheme, "http")  == 0)))
        snprintf(port_part, sizeof(port_part), ":%u", port);

    char cmd[1280];
    if (is_anthropic && api_key && api_key[0]) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'x-api-key: %s' "
            "-H 'anthropic-version: 2023-06-01' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, api_key, tmppath);
    } else if (api_key && api_key[0]) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'Authorization: Bearer %s' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, api_key, tmppath);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, tmppath);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmppath); return -1; }

    char resp[8192];
    size_t total = 0, n;
    while ((n = fread(resp + total, 1, sizeof(resp) - 1 - total, fp)) > 0)
        total += n;
    resp[total] = '\0';
    pclose(fp);
    unlink(tmppath);

    if (total == 0) return -1;

    /* Parse response: OpenAI returns "content":"<string>",
       Anthropic returns "content":[{"type":"text","text":"<string>"}] */
    char *p = strstr(resp, "\"content\":");
    if (!p) return -1;
    p += 10;
    while (*p == ' ') p++;
    if (*p == '[') {
        /* Anthropic array format — find "text" field inside first block */
        p = strstr(p, "\"text\":");
        if (!p) return -1;
        p += 7;
        while (*p == ' ') p++;
        if (*p != '"') return -1;
        p++;
    } else if (*p == '"') {
        p++;
    } else {
        return -1;
    }

    int i = 0;
    while (*p && *p != '"' && i < (int)out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';

    /* Thinking/reasoning models (e.g. nml-think-v2) put their output in
       "reasoning": rather than "content":. Fall back if content is empty. */
    if (i == 0) {
        char *r = strstr(resp, "\"reasoning\":");
        if (r) {
            r += 12;
            while (*r == ' ') r++;
            if (*r == '"') {
                r++;
                i = 0;
                while (*r && *r != '"' && i < (int)out_sz - 1) {
                    if (*r == '\\' && *(r + 1)) r++;
                    out_buf[i++] = *r++;
                }
                out_buf[i] = '\0';
            }
        }
    }
    return i;
}

static int llm_complete(const char *prompt, char *out_buf, size_t out_sz)
{
    return llm_curl(g_llm_host, g_llm_port, g_llm_path,
                    g_llm_api_key, g_llm_model,
                    g_llm_provider, prompt, 256, out_buf, out_sz);
}

static int think_complete(const char *prompt, char *out_buf, size_t out_sz)
{
    /* Think model is always local (OpenAI-compatible mlx_lm server).
       Falls back to external LLM if no think host is configured. */
    const char *host  = g_think_host[0] ? g_think_host : g_llm_host;
    uint16_t    port  = g_think_host[0] ? g_think_port  : g_llm_port;
    const char *path  = g_think_host[0] ? g_think_path  : g_llm_path;
    const char *model = g_think_model[0] ? g_think_model : g_llm_model;
    const char *prov  = g_think_host[0] ? "openai" : g_llm_provider;
    return llm_curl(host, port, path,
                    g_llm_api_key, model,
                    prov, prompt, 512, out_buf, out_sz);
}

/* Local NML code model — writes the spec given enriched context.
   Always OpenAI-compatible (mlx_lm server). */
static int code_complete(const char *prompt, char *out_buf, size_t out_sz)
{
    return llm_curl(g_code_host, g_code_port, g_code_path,
                    NULL, g_code_model, "openai",
                    prompt, 256, out_buf, out_sz);
}

/*
 * Append one JSON line to the assessment log file (if configured).
 * Format: {"ts":N,"phash":"...","weighted_mean":F,"confidence":F,"votes":N}
 */
static void append_assessment_log(const OracleSession *s)
{
    if (g_log_file[0] == '\0') return;
    FILE *f = fopen(g_log_file, "a");
    if (!f) return;
    fprintf(f,
        "{\"ts\":%ld,\"phash\":\"%s\","
        "\"raw_mean\":%.4f,\"weighted_mean\":%.4f,"
        "\"confidence\":%.4f,\"votes\":%d}\n",
        (long)time(NULL), s->phash,
        s->raw_mean, s->weighted_mean,
        s->confidence, s->count);
    fclose(f);
}

/*
 * Generate a program specification and publish to nml/spec.
 * Uses two-stage Think→Code pipeline if both LLM hosts are configured,
 * falls back to Code-only or template otherwise.
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

    /* Build rich per-agent context (safe for embedding in LLM prompts) */
    char agent_ctx[512] = {0};
    int ctx_pos = 0;
    for (int i = 0; i < g_stats.count && ctx_pos < (int)sizeof(agent_ctx) - 48; i++) {
        const AgentStat *st = &g_stats.entries[i];
        float out_rate = (st->vote_count > 0)
                         ? (float)st->outlier_count / (float)st->vote_count
                         : 0.0f;
        ctx_pos += snprintf(agent_ctx + ctx_pos,
                            sizeof(agent_ctx) - (size_t)ctx_pos,
                            "%s:conf=%.2f,out=%.0f%%; ",
                            st->name, st->confidence, out_rate * 100.0f);
    }

    char spec[1024];
    int  spec_len = 0;
    int  used_llm   = 0;
    int  used_think = 0;

    if (g_llm_host[0] != '\0' || g_code_host[0] != '\0') {
        /* Stage 1 (optional): External cloud model provides deep context */
        char external_out[512] = {0};
        if (g_llm_host[0] != '\0' && g_code_host[0] != '\0') {
            char ext_prompt[768];
            snprintf(ext_prompt, sizeof(ext_prompt),
                "NML collective: %d peers, %d programs assessed, "
                "avg_confidence=%.2f. Per-agent: %s"
                "What NML program type would best probe collective health?",
                g_peers.count, assessed, avg_confidence, agent_ctx);
            llm_complete(ext_prompt, external_out, sizeof(external_out));
        }

        /* Stage 2 (optional): Internal think model reasons about NML structure */
        char think_out[640] = {0};
        if (g_think_host[0] != '\0') {
            char think_prompt[768];
            snprintf(think_prompt, sizeof(think_prompt),
                "NML collective: %d peers, avg_confidence=%.2f. Per-agent: %s"
                "%s%s"
                "What type of NML program would best probe collective health?",
                g_peers.count, avg_confidence, agent_ctx,
                external_out[0] ? " Analysis: " : "",
                external_out[0] ? external_out : "");
            if (think_complete(think_prompt, think_out, sizeof(think_out)) > 0)
                used_think = 1;
        }

        /* Stage 3: Code model (or external LLM if code not set) writes the spec */
        char prompt[1024];
        const char *reasoning = think_out[0] ? think_out
                              : external_out[0] ? external_out : NULL;
        if (reasoning) {
            snprintf(prompt, sizeof(prompt),
                "Analysis: %.380s "
                "Write a concise NML program spec (1-2 sentences) for "
                "%d agents with avg_confidence %.2f.",
                reasoning, g_peers.count, avg_confidence);
        } else {
            snprintf(prompt, sizeof(prompt),
                "The NML collective has %d active agents and %d assessed "
                "programs with average confidence %.2f. Per-agent: %s"
                "Suggest a concise NML program spec (1-2 sentences) that "
                "probes collective health.",
                g_peers.count, assessed, avg_confidence, agent_ctx);
        }

        char content[768];
        int spec_ok = g_code_host[0] != '\0'
            ? code_complete(prompt, content, sizeof(content))
            : llm_complete(prompt, content, sizeof(content));
        if (spec_ok > 0) {
            spec_len = snprintf(spec, sizeof(spec),
                "{\"source\":\"llm\",\"assessed\":%d,"
                 "\"avg_confidence\":%.4f,"
                 "\"think\":%s,\"spec\":\"%s\"}",
                assessed, avg_confidence,
                used_think ? "true" : "false",
                content);
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

    /* Encode as MSG_SPEC so the Architect's msg_parse path accepts it.
     * Previously this used raw mqtt_publish, which made the Architect's
     * msg_parse silently reject these packets — the Oracle→Architect loop
     * was effectively broken. */
    {
        uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
        int pkt_len = msg_encode(pkt, sizeof(pkt), MSG_SPEC,
                                 g_agent_name, g_http_port, spec);
        if (pkt_len > 0)
            mqtt_transport_publish(&g_mqtt, MSG_SPEC, pkt, (size_t)pkt_len);
    }
    printf("[oracle] published spec (assessed=%d avg_conf=%.3f llm=%d think=%d)\n",
           assessed, avg_confidence, used_llm, used_think);
}

/* ── HTTP server ─────────────────────────────────────────────────────── */

static compat_socket_t g_http_fd = COMPAT_INVALID_SOCKET;

/* http_listen lives in edge/http_util.c */

/* http_send, http_send_html live in edge/http_util.c (see aliases above). */

static void handle_http(compat_socket_t cfd)
{
    char req[1024];
    int n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) { compat_close_socket(cfd); return; }
    req[n] = '\0';

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        compat_close_socket(cfd); return;
    }

    /* GET / — role-tailored landing page */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send_html(cfd, (const char *)ui_html, (size_t)ui_html_len);
        compat_close_socket(cfd); return;
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        char body[512];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"peers\":%d,\"sessions\":%d,\"agents\":%d,"
             "\"llm\":%s,\"think\":%s,\"log\":%s}",
            g_agent_name, g_peers.count, g_oracle.count, g_stats.count,
            g_llm_host[0]   ? "true" : "false",
            g_think_host[0] ? "true" : "false",
            g_log_file[0]   ? "true" : "false");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
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
        compat_close_socket(cfd); return;
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
            compat_close_socket(cfd); return;
        }
        http_send(cfd, 404, "{\"error\":\"not found\"}");
        compat_close_socket(cfd); return;
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
        compat_close_socket(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /*
     * POST /data/vote  body: {"hash":"<16hex>","approve":true,"reason":"..."}
     * Allows external callers to trigger a data-quality vote via Oracle.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/data/vote") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) { http_send(cfd, 400, "{\"error\":\"no body\"}"); compat_close_socket(cfd); return; }
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
            compat_close_socket(cfd); return;
        }
        publish_data_vote(hash, approve, reason);
        http_send(cfd, 200, "{\"ok\":true}");
        compat_close_socket(cfd); return;
    }

    /*
     * POST /spec  body: {"intent":"..."}
     *
     * Wrap the intent in a spec JSON and publish as MSG_SPEC so the Architect
     * picks it up from nml/spec. This is the Oracle→Architect trigger: the
     * Oracle decides what the collective should build, the Architect builds
     * the NML program from it.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/spec") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;

        char intent[512] = {0};
        {
            char *p = strstr(body_start, "\"intent\":");
            if (p) {
                p += 9;
                while (*p == ' ' || *p == '"') p++;
                int j = 0;
                while (*p && *p != '"' && j < (int)sizeof(intent) - 1) {
                    if (*p == '\\' && *(p + 1)) p++;   /* skip escape */
                    intent[j++] = *p++;
                }
                intent[j] = '\0';
            }
        }
        if (intent[0] == '\0') {
            http_send(cfd, 400, "{\"error\":\"intent required\"}");
            compat_close_socket(cfd); return;
        }

        /* Wrap the intent in the standard spec envelope. The Architect keys
         * off the "spec" field and uses "source" for attribution. */
        char spec_json[640];
        int  sj_len = snprintf(spec_json, sizeof(spec_json),
            "{\"source\":\"oracle-http\",\"spec\":\"%s\"}", intent);
        (void)sj_len;

        uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
        int pkt_len = msg_encode(pkt, sizeof(pkt), MSG_SPEC,
                                 g_agent_name, g_http_port, spec_json);
        if (pkt_len < 0) {
            http_send(cfd, 500, "{\"error\":\"spec too large\"}");
            compat_close_socket(cfd); return;
        }
        mqtt_transport_publish(&g_mqtt, MSG_SPEC, pkt, (size_t)pkt_len);

        printf("[oracle] /spec queued: %.60s\n", intent);
        http_send(cfd, 201, "{\"ok\":true,\"queued\":true}");
        compat_close_socket(cfd); return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        http_send(cfd, 200, "{}");
        compat_close_socket(cfd); return;
    }

    http_send(cfd, 404, "{\"error\":\"not found\"}");
    compat_close_socket(cfd);
}

/* ── CLI usage ───────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--name NAME] [--broker HOST] [--broker-port PORT]\n"
        "          [--port HTTP_PORT] [--quorum N]\n"
        "          [--llm-host HOST] [--llm-port PORT] [--llm-path PATH]\n"
        "          [--llm-api-key KEY] [--llm-model MODEL]\n"
        "          [--think-host HOST] [--think-port PORT] [--think-path PATH]\n"
        "          [--think-model MODEL]\n"
        "          [--log-file PATH]\n",
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
        else if (strcmp(argv[i], "--think-host") == 0 && i + 1 < argc)
            strncpy(g_think_host, argv[++i], sizeof(g_think_host) - 1);
        else if (strcmp(argv[i], "--think-port") == 0 && i + 1 < argc)
            g_think_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--think-path") == 0 && i + 1 < argc)
            strncpy(g_think_path, argv[++i], sizeof(g_think_path) - 1);
        else if (strcmp(argv[i], "--think-model") == 0 && i + 1 < argc)
            strncpy(g_think_model, argv[++i], sizeof(g_think_model) - 1);
        else if (strcmp(argv[i], "--llm-api-key") == 0 && i + 1 < argc)
            strncpy(g_llm_api_key, argv[++i], sizeof(g_llm_api_key) - 1);
        else if (strcmp(argv[i], "--llm-model") == 0 && i + 1 < argc)
            strncpy(g_llm_model, argv[++i], sizeof(g_llm_model) - 1);
        else if (strcmp(argv[i], "--llm-provider") == 0 && i + 1 < argc)
            strncpy(g_llm_provider, argv[++i], sizeof(g_llm_provider) - 1);
        else if (strcmp(argv[i], "--code-host") == 0 && i + 1 < argc)
            strncpy(g_code_host, argv[++i], sizeof(g_code_host) - 1);
        else if (strcmp(argv[i], "--code-port") == 0 && i + 1 < argc)
            g_code_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--code-path") == 0 && i + 1 < argc)
            strncpy(g_code_path, argv[++i], sizeof(g_code_path) - 1);
        else if (strcmp(argv[i], "--code-model") == 0 && i + 1 < argc)
            strncpy(g_code_model, argv[++i], sizeof(g_code_model) - 1);
        else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc)
            strncpy(g_log_file, argv[++i], sizeof(g_log_file) - 1);
        else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        }
    }

    compat_winsock_init();

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
    if (g_http_fd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[oracle] failed to bind HTTP on port %u\n", g_http_port);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }
    printf("[oracle] HTTP API on port %u\n", g_http_port);
    printf("[oracle] quorum=%d  broker=%s:%u%s%s%s\n",
           g_quorum, g_broker_host, g_broker_port,
           g_llm_host[0]   ? "  llm=enabled"   : "",
           g_think_host[0] ? "  think=enabled" : "",
           g_log_file[0]   ? "  log=enabled"   : "");

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
        if (select(COMPAT_SELECT_NFDS(g_http_fd), &rfds, NULL, NULL, &tv) > 0 &&
            FD_ISSET(g_http_fd, &rfds)) {
            compat_socket_t cfd = accept(g_http_fd, NULL, NULL);
            if (cfd != COMPAT_INVALID_SOCKET) handle_http(cfd);
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

            if (type == MSG_RESULT || type == MSG_VOTE)
                handle_result(pname, payload);
            /* MSG_VOTE uses the same phash:score payload as MSG_RESULT —
             * governance votes from sentient/enforcer/architect feed into
             * the same weighted z-score pipeline as worker execution results. */
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
    if (g_http_fd != COMPAT_INVALID_SOCKET) compat_close_socket(g_http_fd);
    compat_winsock_cleanup();
    return 0;
}
