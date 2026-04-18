/*
 * emissary_agent.c — NML Collective Emissary.
 *
 * External boundary: the single contact point between the NML collective and
 * the outside world.  Internal agents are never directly reachable; all
 * external traffic enters here.
 *
 * Architecture:
 *   - MQTT subscriber: tracks peers and collects NML wire-format events
 *   - HTTP server: unified REST API for external operators and systems
 *   - Proxy: forwards inbound data to Sentient, spec requests via MSG_SPEC
 *   - Webhooks: fires registered URLs when new Oracle assessments arrive
 *   - Rate limiting: per-IP request counter (RATE_MAX_RPS per second)
 *   - Auth: optional Bearer token on all external routes
 *
 * Usage:
 *   ./emissary_agent [--name NAME] [--broker HOST] [--broker-port PORT]
 *                    [--port HTTP_PORT] [--api-key TOKEN]
 *                    [--sentient HOST] [--sentient-port PORT]
 *                    [--oracle HOST]   [--oracle-port PORT]
 *
 * Inputs:  nml/# (MQTT, NML wire-format messages for peer tracking)
 * Outputs: nml/spec (MSG_SPEC to Architect), HTTP proxy to Sentient
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/mqtt_transport.h"

/* Direct MQTT publish for MSG_SPEC */
#include "../../edge/mqtt/mqtt.h"

#include "compat.h"
#include "../../edge/http_util.h"

/* Embedded landing page, generated from ui.html via `xxd -i`. */
#include "ui.html.h"

#define http_send  http_send_json
#define json_str   http_json_str

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../nml/runtime/nml_crypto.h"
#pragma GCC diagnostic pop

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_WEBHOOKS          32
#define MAX_RESULTS           128
#define MAX_RATE_ENTRIES      64
#define RATE_MAX_RPS          10       /* max requests per second per IP */
#define HTTP_BUF_SZ           16384
#define HEARTBEAT_INTERVAL_S  5
#define STALE_PEER_S          30
#define POLL_INTERVAL_S       5        /* poll Oracle/Sentient every N seconds */
#define WEBHOOK_TIMEOUT_S     5

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    int    id;
    char   url[512];
    char   events[128];   /* comma-separated: "result,enforce,announce" */
    int    active;
} Webhook;

typedef struct {
    char  phash[17];
    float weighted_mean;
    float confidence;
    int   vote_count;
    time_t arrived_at;
    int   webhook_fired;
} ResultEntry;

typedef struct {
    char   ip[46];
    int    count;
    time_t window_start;
} RateEntry;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static MQTTTransport g_mqtt;
static PeerTable     g_peers;

static Webhook     g_webhooks[MAX_WEBHOOKS];
static int         g_webhook_count = 0;
static int         g_webhook_next_id = 1;

static ResultEntry g_results[MAX_RESULTS];
static int         g_results_count = 0;
static int         g_results_next  = 0;

static RateEntry   g_rate[MAX_RATE_ENTRIES];
static int         g_rate_count = 0;

static const char *g_agent_name     = "emissary";
static char g_broker_host[128]      = "127.0.0.1";
static uint16_t g_broker_port       = 1883;
static uint16_t g_http_port         = 9000;
static char g_api_key[128]          = {0};   /* empty = no auth required */
static char g_sentient_host[128]    = "127.0.0.1";
static uint16_t g_sentient_port     = 9001;
static char g_oracle_host[128]      = "127.0.0.1";
static uint16_t g_oracle_port       = 9002;

/* LLM inference endpoints for POST /infer */
static char g_think_host[128]       = {0};
static uint16_t g_think_port        = 443;
static char g_think_model[128]      = {0};
static char g_code_host[128]        = {0};
static uint16_t g_code_port         = 443;
static char g_code_model[128]       = "openai/gpt-4o-mini";
static char g_llm_path[256]         = "/api/v1/chat/completions";
static char g_llm_api_key[256]      = {0};

/* ── Rate limiting ───────────────────────────────────────────────────── */

/* Returns 1 if the IP is allowed, 0 if rate-limited. */
static int rate_check(const char *ip)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_rate_count; i++) {
        if (strcmp(g_rate[i].ip, ip) != 0) continue;
        if (now - g_rate[i].window_start >= 1) {
            g_rate[i].window_start = now;
            g_rate[i].count = 1;
            return 1;
        }
        if (g_rate[i].count >= RATE_MAX_RPS) return 0;
        g_rate[i].count++;
        return 1;
    }
    /* New IP */
    int slot = (g_rate_count < MAX_RATE_ENTRIES)
               ? g_rate_count++ : (int)(now % MAX_RATE_ENTRIES);
    snprintf(g_rate[slot].ip, sizeof(g_rate[slot].ip), "%s", ip);
    g_rate[slot].count        = 1;
    g_rate[slot].window_start = now;
    return 1;
}

/* ── Simple JSON helpers ─────────────────────────────────────────────── */
/* json_str lives in edge/http_util.c (see alias above). */

static float json_float(const char *json, const char *key, float def)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ') p++;
    return strtof(p, NULL);
}

/* ── Internal HTTP client ────────────────────────────────────────────── */

/*
 * Perform a GET/POST to an internal agent.
 * Stores the response body (after headers) in resp_buf.
 * Returns response body length, or -1 on error.
 */
static int internal_http(const char *host, uint16_t port,
                          const char *method, const char *path,
                          const char *body,   /* NULL for GET */
                          char *resp_buf, size_t resp_sz)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COMPAT_INVALID_SOCKET) return -1;

    /* Short timeout via select */
    struct timeval tv = {WEBHOOK_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        compat_close_socket(fd); return -1;
    }

    char req[4096];
    int req_len;
    if (body && strlen(body) > 0) {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            method, path, host, port, strlen(body), body);
    } else {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "Connection: close\r\n\r\n",
            method, path, host, port);
    }
    send(fd, req, (size_t)req_len, 0);

    char raw[HTTP_BUF_SZ];
    int rn = recv(fd, raw, sizeof(raw) - 1, 0);
    compat_close_socket(fd);
    if (rn <= 0) return -1;
    raw[rn] = '\0';

    char *hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end) return -1;
    hdr_end += 4;

    size_t body_len = (size_t)(rn - (hdr_end - raw));
    if (body_len >= resp_sz) body_len = resp_sz - 1;
    memcpy(resp_buf, hdr_end, body_len);
    resp_buf[body_len] = '\0';
    return (int)body_len;
}

/* ── LLM call helper ─────────────────────────────────────────────────── */

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
 * POST to an OpenAI-compatible endpoint via curl (handles HTTPS + auth).
 * system_msg may be NULL.  Extracts first "content" value into out_buf.
 * Returns chars written, or -1 on failure.
 */
static int llm_call(const char *host, uint16_t port, const char *path,
                    const char *system_msg, const char *user_msg,
                    char *out_buf, size_t out_sz)
{
    if (!host || host[0] == '\0') return -1;

    char esc_sys[1024] = {0}, esc_usr[2048];
    if (system_msg) json_escape(system_msg, esc_sys, sizeof(esc_sys));
    json_escape(user_msg, esc_usr, sizeof(esc_usr));

    char body[4096];
    if (system_msg && system_msg[0]) {
        snprintf(body, sizeof(body),
            "{\"model\":\"%s\","
             "\"messages\":["
               "{\"role\":\"system\",\"content\":\"%s\"},"
               "{\"role\":\"user\",\"content\":\"%s\"}"
             "],"
             "\"max_tokens\":512}",
            g_code_model, esc_sys, esc_usr);
    } else {
        snprintf(body, sizeof(body),
            "{\"model\":\"%s\","
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
             "\"max_tokens\":512}",
            g_code_model, esc_usr);
    }

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

    char cmd[1024];
    if (g_llm_api_key[0]) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'Authorization: Bearer %s' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, g_llm_api_key, tmppath);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, tmppath);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmppath); return -1; }

    char resp[HTTP_BUF_SZ];
    size_t total = 0, n;
    while ((n = fread(resp + total, 1, sizeof(resp) - 1 - total, fp)) > 0)
        total += n;
    resp[total] = '\0';
    pclose(fp);
    unlink(tmppath);

    if (total == 0) return -1;

    char *p = strstr(resp, "\"content\":");
    if (!p) return -1;
    p += 10;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;

    int i = 0;
    while (*p && *p != '"' && i < (int)out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    return i;
}

/* ── HMAC-SHA256 helper ──────────────────────────────────────────────── */

/*
 * Compute HMAC-SHA256(key, data) and write lowercase hex to out (65 bytes).
 * Uses SHA256_CTX from nml_crypto.h.
 */
static void hmac_sha256_hex(const char *key, const uint8_t *data, size_t data_len,
                            char out[65])
{
    uint8_t k[64];
    uint8_t ipad[64], opad[64];
    size_t klen = strlen(key);

    memset(k, 0, sizeof(k));
    if (klen <= 64) {
        memcpy(k, key, klen);
    } else {
        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, (const uint8_t *)key, klen);
        sha256_final(&ctx, k);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36u;
        opad[i] = k[i] ^ 0x5Cu;
    }

    uint8_t inner[32];
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner);

    uint8_t digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, digest);

    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = '\0';
}

/* ── Webhook delivery ────────────────────────────────────────────────── */

static void webhook_fire(const char *event, const char *payload)
{
    for (int i = 0; i < g_webhook_count; i++) {
        Webhook *w = &g_webhooks[i];
        if (!w->active) continue;
        if (strstr(w->events, event) == NULL &&
            strstr(w->events, "all") == NULL) continue;

        /* Parse host and path from URL (http://host:port/path) */
        char url[512];
        snprintf(url, sizeof(url), "%s", w->url);
        char *p = strstr(url, "://");
        if (!p) continue;
        p += 3;
        char *slash = strchr(p, '/');
        char path[512] = "/";
        if (slash) {
            snprintf(path, sizeof(path), "%s", slash);
            *slash = '\0';
        }
        /* Parse host:port */
        char whost[128] = {0};
        uint16_t wport  = 80;
        char *colon = strchr(p, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - p);
            if (hlen >= sizeof(whost)) hlen = sizeof(whost) - 1;
            memcpy(whost, p, hlen);
            wport = (uint16_t)atoi(colon + 1);
        } else {
            snprintf(whost, sizeof(whost), "%s", p);
        }

        char body[2048];
        snprintf(body, sizeof(body),
            "{\"event\":\"%s\",\"payload\":%s,\"source\":\"%s\"}",
            event, payload, g_agent_name);

        /* Compute HMAC-SHA256 signature if an API key is set */
        char sig_header[128] = {0};
        if (g_api_key[0] != '\0') {
            char hex[65];
            hmac_sha256_hex(g_api_key,
                            (const uint8_t *)body, strlen(body),
                            hex);
            snprintf(sig_header, sizeof(sig_header), "sha256=%s", hex);
        }

        /* Retry up to 3 times with exponential back-off (1s / 2s / 4s) */
        int delivered = 0;
        for (int attempt = 0; attempt < 3 && !delivered; attempt++) {
            if (attempt > 0) {
                struct timeval delay;
                delay.tv_sec  = 1 << (attempt - 1);   /* 1, 2 */
                delay.tv_usec = 0;
                select(0, NULL, NULL, NULL, &delay);
            }

            struct sockaddr_in waddr;
            memset(&waddr, 0, sizeof(waddr));
            waddr.sin_family      = AF_INET;
            waddr.sin_port        = htons(wport);
            waddr.sin_addr.s_addr = inet_addr(whost);

            compat_socket_t wfd = socket(AF_INET, SOCK_STREAM, 0);
            if (wfd == COMPAT_INVALID_SOCKET) continue;
            struct timeval tv = {WEBHOOK_TIMEOUT_S, 0};
            setsockopt(wfd, SOL_SOCKET, SO_RCVTIMEO,
                       COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));
            setsockopt(wfd, SOL_SOCKET, SO_SNDTIMEO,
                       COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));

            if (connect(wfd, (struct sockaddr *)&waddr, sizeof(waddr)) < 0) {
                compat_close_socket(wfd); continue;
            }

            char http_req[4096];
            int req_len;
            if (sig_header[0] != '\0') {
                req_len = snprintf(http_req, sizeof(http_req),
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s:%u\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "X-NML-Signature: %s\r\n"
                    "Connection: close\r\n\r\n%s",
                    path, whost, wport, strlen(body), sig_header, body);
            } else {
                req_len = snprintf(http_req, sizeof(http_req),
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s:%u\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n%s",
                    path, whost, wport, strlen(body), body);
            }
            send(wfd, http_req, (size_t)req_len, 0);

            char wbuf[256];
            int rn = recv(wfd, wbuf, sizeof(wbuf) - 1, 0);
            compat_close_socket(wfd);

            /* Accept any HTTP 2xx as success */
            if (rn > 0) {
                wbuf[rn] = '\0';
                if (strstr(wbuf, "HTTP/1") && strstr(wbuf, " 2"))
                    delivered = 1;
            }
        }
        if (!delivered)
            fprintf(stderr, "[emissary] webhook %d delivery failed after 3 attempts\n",
                    w->id);
    }
}

/* ── Oracle polling ──────────────────────────────────────────────────── */

/*
 * Poll Oracle for new assessments and add them to g_results.
 * Fires webhooks for newly seen phashes.
 */
static void poll_oracle(void)
{
    char resp[HTTP_BUF_SZ];
    int n = internal_http(g_oracle_host, g_oracle_port,
                          "GET", "/assessments", NULL,
                          resp, sizeof(resp));
    if (n <= 0) return;

    /* Parse JSON array of assessments (simple scan for "phash" fields) */
    const char *p = resp;
    while ((p = strstr(p, "\"phash\":\"")) != NULL) {
        p += 9;
        char phash[17] = {0};
        int j = 0;
        while (*p && *p != '"' && j < 16) phash[j++] = *p++;
        phash[j] = '\0';
        if (j != 16) continue;

        /* Check if already known */
        int known = 0;
        for (int i = 0; i < g_results_count; i++) {
            if (strcmp(g_results[i].phash, phash) == 0) {
                known = 1; break;
            }
        }
        if (known) continue;

        /* Extract full assessment JSON for this phash */
        char detail_path[64];
        snprintf(detail_path, sizeof(detail_path), "/assessments/%s", phash);
        char detail[2048];
        if (internal_http(g_oracle_host, g_oracle_port,
                          "GET", detail_path, NULL,
                          detail, sizeof(detail)) <= 0) continue;

        /* Store in results ring */
        ResultEntry *e;
        if (g_results_count < MAX_RESULTS) {
            e = &g_results[g_results_count++];
        } else {
            e = &g_results[g_results_next % MAX_RESULTS];
            g_results_next++;
        }
        snprintf(e->phash, sizeof(e->phash), "%s", phash);
        e->weighted_mean = json_float(detail, "weighted_mean", 0.0f);
        e->confidence    = json_float(detail, "confidence",    0.0f);
        e->vote_count    = 0;
        e->arrived_at    = time(NULL);
        e->webhook_fired = 0;

        /* Fire webhook */
        char event_payload[256];
        snprintf(event_payload, sizeof(event_payload),
            "{\"phash\":\"%s\",\"weighted_mean\":%.4f,\"confidence\":%.4f}",
            phash, e->weighted_mean, e->confidence);
        webhook_fire("result", event_payload);
        e->webhook_fired = 1;

        printf("[emissary] new result  phash=%s  mean=%.4f  conf=%.4f\n",
               phash, e->weighted_mean, e->confidence);
    }
}

/* ── HTTP server helpers ─────────────────────────────────────────────── */

static compat_socket_t g_http_fd = COMPAT_INVALID_SOCKET;

/* http_listen, http_send, http_send_html live in edge/http_util.c */

/*
 * Check Bearer token authentication.
 * Returns 1 if auth passes (or no key configured), 0 if rejected.
 */
static int check_auth(const char *req)
{
    if (g_api_key[0] == '\0') return 1;   /* no auth required */
    char needle[256];
    snprintf(needle, sizeof(needle), "Bearer %s", g_api_key);
    return strstr(req, needle) != NULL ? 1 : 0;
}

/* ── Route handlers ──────────────────────────────────────────────────── */

static void handle_http(compat_socket_t cfd, const char *client_ip)
{
    char req[HTTP_BUF_SZ];
    int n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) { compat_close_socket(cfd); return; }
    req[n] = '\0';

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        compat_close_socket(cfd); return;
    }

    /* Rate limiting */
    if (!rate_check(client_ip)) {
        http_send(cfd, 429, "{\"error\":\"rate limit exceeded\"}");
        compat_close_socket(cfd); return;
    }

    /* OPTIONS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        http_send(cfd, 200, "{}");
        compat_close_socket(cfd); return;
    }

    /* GET / — role landing page (served before auth so browsers can reach it) */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send_html(cfd, (const char *)ui_html, (size_t)ui_html_len);
        compat_close_socket(cfd); return;
    }

    /* Authentication */
    if (!check_auth(req)) {
        http_send(cfd, 401, "{\"error\":\"unauthorized\"}");
        compat_close_socket(cfd); return;
    }

    /* ── GET /health ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        char body[512];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"peers\":%d,\"results\":%d,\"webhooks\":%d}",
            g_agent_name, g_peers.count, g_results_count, g_webhook_count);
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* ── GET /status ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) {
        char peers_json[HTTP_BUF_SZ / 2];
        peer_list_json(&g_peers, peers_json, sizeof(peers_json));

        char body[HTTP_BUF_SZ];
        snprintf(body, sizeof(body),
            "{\"name\":\"%s\","
             "\"peers\":%s,"
             "\"results_count\":%d,"
             "\"webhooks_active\":%d}",
            g_agent_name, peers_json, g_results_count, g_webhook_count);
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* ── GET /peers ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* ── GET /results ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/results") == 0) {
        char body[HTTP_BUF_SZ];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < g_results_count; i++) {
            const ResultEntry *e = &g_results[i];
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"phash\":\"%s\","
                 "\"weighted_mean\":%.4f,"
                 "\"confidence\":%.4f}",
                e->phash, e->weighted_mean, e->confidence);
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* ── GET /results/<phash> — proxy to Oracle ── */
    if (strcmp(method, "GET") == 0 && strncmp(path, "/results/", 9) == 0) {
        char oracle_path[512];
        snprintf(oracle_path, sizeof(oracle_path),
                 "/assessments/%s", path + 9);
        char resp[4096];
        if (internal_http(g_oracle_host, g_oracle_port,
                          "GET", oracle_path, NULL,
                          resp, sizeof(resp)) > 0) {
            http_send(cfd, 200, resp);
        } else {
            http_send(cfd, 404, "{\"error\":\"not found\"}");
        }
        compat_close_socket(cfd); return;
    }

    /* ── POST /data — ingest external data, proxy to Sentient ── */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/data") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;

        char resp[1024];
        if (internal_http(g_sentient_host, g_sentient_port,
                          "POST", "/data/submit", body_start,
                          resp, sizeof(resp)) > 0) {
            http_send(cfd, 201, resp);
        } else {
            http_send(cfd, 502, "{\"error\":\"sentient unavailable\"}");
        }
        compat_close_socket(cfd); return;
    }

    /* ── GET /quarantine — proxy to Sentient ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/quarantine") == 0) {
        char resp[HTTP_BUF_SZ];
        if (internal_http(g_sentient_host, g_sentient_port,
                          "GET", "/data/quarantine", NULL,
                          resp, sizeof(resp)) > 0) {
            http_send(cfd, 200, resp);
        } else {
            http_send(cfd, 502, "{\"error\":\"sentient unavailable\"}");
        }
        compat_close_socket(cfd); return;
    }

    /* ── POST /spec — request program generation via Architect ── */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/spec") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;

        char spec[512] = {0};
        if (json_str(body_start, "spec", spec, sizeof(spec)) <= 0) {
            http_send(cfd, 400, "{\"error\":\"spec required\"}");
            compat_close_socket(cfd); return;
        }

        /* Encode as MSG_SPEC and publish to nml/spec */
        char spec_json[640];
        snprintf(spec_json, sizeof(spec_json),
            "{\"source\":\"emissary\",\"spec\":\"%s\"}", spec);

        uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
        int pkt_len = msg_encode(pkt, sizeof(pkt), MSG_SPEC,
                                 g_agent_name, g_http_port, spec_json);
        if (pkt_len > 0)
            mqtt_transport_publish(&g_mqtt, MSG_SPEC, pkt, (size_t)pkt_len);

        http_send(cfd, 201, "{\"ok\":true,\"queued\":true}");
        printf("[emissary] spec queued: %.60s\n", spec);
        compat_close_socket(cfd); return;
    }

    /* ── GET /webhooks ── */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/webhooks") == 0) {
        char body[HTTP_BUF_SZ];
        int pos = snprintf(body, sizeof(body), "[");
        int first = 1;
        for (int i = 0; i < g_webhook_count; i++) {
            Webhook *w = &g_webhooks[i];
            if (!w->active) continue;
            if (!first) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"id\":%d,\"url\":\"%s\",\"events\":\"%s\"}",
                w->id, w->url, w->events);
            first = 0;
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* ── POST /webhooks ── */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/webhooks") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;

        char url[512] = {0};
        char events[128] = "all";
        json_str(body_start, "url", url, sizeof(url));
        json_str(body_start, "events", events, sizeof(events));

        if (url[0] == '\0') {
            http_send(cfd, 400, "{\"error\":\"url required\"}");
            compat_close_socket(cfd); return;
        }
        if (g_webhook_count >= MAX_WEBHOOKS) {
            http_send(cfd, 429, "{\"error\":\"webhook limit reached\"}");
            compat_close_socket(cfd); return;
        }

        Webhook *w = &g_webhooks[g_webhook_count++];
        w->id     = g_webhook_next_id++;
        w->active = 1;
        snprintf(w->url,    sizeof(w->url),    "%s", url);
        snprintf(w->events, sizeof(w->events), "%s", events);

        char resp[128];
        snprintf(resp, sizeof(resp), "{\"id\":%d}", w->id);
        http_send(cfd, 201, resp);
        printf("[emissary] webhook %d registered: %s  events=%s\n",
               w->id, url, events);
        compat_close_socket(cfd); return;
    }

    /*
     * POST /infer  body: {"prompt":"..."}
     * Two-stage pipeline: Think model reasons, Code model generates NML spec.
     * Publishes result as MSG_SPEC for Architect, returns reasoning + spec.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/infer") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;

        char prompt[512] = {0};
        if (json_str(body_start, "prompt", prompt, sizeof(prompt)) <= 0) {
            http_send(cfd, 400, "{\"error\":\"prompt required\"}");
            compat_close_socket(cfd); return;
        }

        /* Stage 1: Think model — reason about the intent */
        char reasoning[768] = {0};
        if (g_think_host[0] != '\0') {
            char think_prompt[640];
            snprintf(think_prompt, sizeof(think_prompt),
                "User intent: %.400s "
                "Analyze what NML operations are needed. "
                "Be concise (2-3 sentences).",
                prompt);
            llm_call(g_think_host, g_think_port, g_llm_path,
                     NULL, think_prompt, reasoning, sizeof(reasoning));
        }

        /* Stage 2: Code model — generate NML spec text */
        char spec_text[512] = {0};
        if (g_code_host[0] != '\0') {
            char code_prompt[768];
            if (reasoning[0] != '\0') {
                snprintf(code_prompt, sizeof(code_prompt),
                    "Analysis: %.300s Intent: %.200s "
                    "Write a concise NML spec (1-2 sentences).",
                    reasoning, prompt);
            } else {
                snprintf(code_prompt, sizeof(code_prompt),
                    "%.400s Write a concise NML spec (1-2 sentences).", prompt);
            }
            llm_call(g_code_host, g_code_port, g_llm_path,
                     "Generate a minimal NML program spec. "
                     "Output only the spec text.",
                     code_prompt, spec_text, sizeof(spec_text));
        }

        /* Publish as MSG_SPEC for Architect to process */
        if (spec_text[0] != '\0') {
            char spec_json[640];
            snprintf(spec_json, sizeof(spec_json),
                "{\"source\":\"emissary-infer\",\"spec\":\"%s\"}", spec_text);
            uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
            int pkt_len = msg_encode(pkt, sizeof(pkt), MSG_SPEC,
                                     g_agent_name, g_http_port, spec_json);
            if (pkt_len > 0)
                mqtt_transport_publish(&g_mqtt, MSG_SPEC, pkt, (size_t)pkt_len);
        }

        char resp_body[1536];
        snprintf(resp_body, sizeof(resp_body),
            "{\"ok\":true,\"reasoning\":\"%s\",\"spec\":\"%s\",\"queued\":%s}",
            reasoning[0]  ? reasoning  : "",
            spec_text[0]  ? spec_text  : "",
            spec_text[0]  ? "true" : "false");
        http_send(cfd, 200, resp_body);
        printf("[emissary] /infer  think=%s code=%s queued=%s\n",
               g_think_host[0] ? "yes" : "no",
               g_code_host[0]  ? "yes" : "no",
               spec_text[0]    ? "yes" : "no");
        compat_close_socket(cfd); return;
    }

    /* ── DELETE /webhooks/<id> ── */
    if (strcmp(method, "DELETE") == 0 &&
        strncmp(path, "/webhooks/", 10) == 0) {
        int target_id = atoi(path + 10);
        for (int i = 0; i < g_webhook_count; i++) {
            if (g_webhooks[i].id == target_id && g_webhooks[i].active) {
                g_webhooks[i].active = 0;
                http_send(cfd, 200, "{\"ok\":true}");
                compat_close_socket(cfd); return;
            }
        }
        http_send(cfd, 404, "{\"error\":\"webhook not found\"}");
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
        "          [--port HTTP_PORT] [--api-key TOKEN]\n"
        "          [--sentient HOST] [--sentient-port PORT]\n"
        "          [--oracle HOST]   [--oracle-port PORT]\n"
        "          [--think-host HOST] [--think-port PORT] [--think-model MODEL]\n"
        "          [--code-host HOST]  [--code-port PORT]  [--code-model MODEL]\n"
        "          [--llm-path PATH]   [--llm-api-key KEY]\n",
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
            snprintf(g_broker_host, sizeof(g_broker_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc)
            g_broker_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_http_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc)
            snprintf(g_api_key, sizeof(g_api_key), "%s", argv[++i]);
        else if (strcmp(argv[i], "--sentient") == 0 && i + 1 < argc)
            snprintf(g_sentient_host, sizeof(g_sentient_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--sentient-port") == 0 && i + 1 < argc)
            g_sentient_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--oracle") == 0 && i + 1 < argc)
            snprintf(g_oracle_host, sizeof(g_oracle_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--oracle-port") == 0 && i + 1 < argc)
            g_oracle_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--think-host") == 0 && i + 1 < argc)
            snprintf(g_think_host, sizeof(g_think_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--think-port") == 0 && i + 1 < argc)
            g_think_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--code-host") == 0 && i + 1 < argc)
            snprintf(g_code_host, sizeof(g_code_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--code-port") == 0 && i + 1 < argc)
            g_code_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--llm-path") == 0 && i + 1 < argc)
            snprintf(g_llm_path, sizeof(g_llm_path), "%s", argv[++i]);
        else if (strcmp(argv[i], "--llm-api-key") == 0 && i + 1 < argc)
            snprintf(g_llm_api_key, sizeof(g_llm_api_key), "%s", argv[++i]);
        else if (strcmp(argv[i], "--think-model") == 0 && i + 1 < argc)
            snprintf(g_think_model, sizeof(g_think_model), "%s", argv[++i]);
        else if (strcmp(argv[i], "--code-model") == 0 && i + 1 < argc)
            snprintf(g_code_model, sizeof(g_code_model), "%s", argv[++i]);
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
    printf("[emissary] identity  machine=%s  node=%s\n",
           g_machine_hash_hex, g_node_id_hex);

    /* ── Init tables ── */
    peer_table_init(&g_peers);
    memset(g_webhooks, 0, sizeof(g_webhooks));
    memset(g_results,  0, sizeof(g_results));
    memset(g_rate,     0, sizeof(g_rate));

    /* ── MQTT ── */
    if (mqtt_transport_init(&g_mqtt, g_broker_host, g_broker_port,
                             g_agent_name, g_http_port,
                             g_identity_payload) != 0) {
        fprintf(stderr, "[emissary] failed to connect to broker %s:%u\n",
                g_broker_host, g_broker_port);
        return 1;
    }

    /* ── HTTP server ── */
    g_http_fd = http_listen(g_http_port);
    if (g_http_fd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[emissary] failed to bind HTTP on port %u\n",
                g_http_port);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }

    printf("[emissary] external API on port %u%s\n",
           g_http_port, g_api_key[0] ? "  (auth required)" : "");
    printf("[emissary] sentient=%s:%u  oracle=%s:%u  broker=%s:%u\n",
           g_sentient_host, g_sentient_port,
           g_oracle_host, g_oracle_port,
           g_broker_host, g_broker_port);
    if (g_think_host[0] || g_code_host[0])
        printf("[emissary] infer pipeline: think=%s:%u  code=%s:%u\n",
               g_think_host[0] ? g_think_host : "(disabled)", g_think_port,
               g_code_host[0]  ? g_code_host  : "(disabled)", g_code_port);

    /* ── Main loop ── */
    time_t last_heartbeat = 0;
    time_t last_poll      = 0;

    while (g_running) {
        time_t now = time(NULL);

        /* Wait up to 1 second for external HTTP connections */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_http_fd, &rfds);
        struct timeval tv = {1, 0};
        if (select(COMPAT_SELECT_NFDS(g_http_fd), &rfds, NULL, NULL, &tv) > 0 &&
            FD_ISSET(g_http_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            compat_socket_t cfd = accept(g_http_fd,
                             (struct sockaddr *)&client_addr, &addr_len);
            if (cfd != COMPAT_INVALID_SOCKET) {
                char client_ip[46];
                inet_ntop(AF_INET, &client_addr.sin_addr,
                          client_ip, sizeof(client_ip));
                handle_http(cfd, client_ip);
            }
        }

        /* Sync MQTT — track collective members via ANNOUNCE/HEARTBEAT */
        mqtt_transport_sync(&g_mqtt, 0);

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

            /* Fire webhooks for enforcement events */
            if (type == MSG_ENFORCE) {
                char event_json[256];
                snprintf(event_json, sizeof(event_json),
                    "{\"agent\":\"%s\",\"payload\":\"%.60s\"}",
                    pname, payload);
                webhook_fire("enforce", event_json);
            }
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
        }

        /* Periodic Oracle polling for new results */
        if (now - last_poll >= POLL_INTERVAL_S) {
            poll_oracle();
            last_poll = now;
        }
    }

    printf("[emissary] shutting down\n");
    mqtt_transport_close(&g_mqtt);
    if (g_http_fd != COMPAT_INVALID_SOCKET) compat_close_socket(g_http_fd);
    compat_winsock_cleanup();
    return 0;
}
