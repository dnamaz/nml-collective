/*
 * architect_agent.c — NML Collective Architect role.
 *
 * Receives program specifications from the Oracle (nml/spec, MSG_SPEC),
 * generates NML programs via template engine (primary path) or LLM HTTP
 * (fallback), validates by dry-run execution, and publishes to nml/program
 * (MSG_PROGRAM) for the Sentient to sign and broadcast to workers.
 *
 * Usage:
 *   ./architect_agent [--name NAME] [--broker HOST] [--broker-port PORT]
 *                     [--port HTTP_PORT]
 *                     [--llm-host HOST] [--llm-port PORT] [--llm-path PATH]
 *
 * Inputs:  nml/spec    — MSG_SPEC from Oracle (JSON payload with "spec" key)
 * Outputs: nml/program — MSG_PROGRAM for Sentient to sign + broadcast
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/mqtt_transport.h"
#include "../../edge/templates.h"
#include "../../edge/nml_exec.h"

/* Direct MQTT publish for custom topics */
#include "../../edge/mqtt/mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_CATALOG_ENTRIES  128
#define HTTP_BUF_SZ          8192
#define HEARTBEAT_INTERVAL_S 5
#define STALE_PEER_S         30

/* Provenance flags */
#define PROV_TEMPLATE 0
#define PROV_LLM      1

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    char phash[17];
    char spec[256];
    int  provenance;        /* PROV_TEMPLATE or PROV_LLM */
    int  template_id;       /* -1 if LLM-generated */
    time_t generated_at;
    int  validated;         /* 1 if dry-run passed */
} CatalogEntry;

typedef struct {
    CatalogEntry entries[MAX_CATALOG_ENTRIES];
    int          count;
    int          next;      /* ring index for eviction */
} ProgramCatalog;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static MQTTTransport g_mqtt;
static PeerTable     g_peers;
static ProgramCatalog g_catalog;

static const char *g_agent_name = "architect";
static char g_broker_host[128]  = "127.0.0.1";
static uint16_t g_broker_port   = 1883;
static uint16_t g_http_port     = 9003;
static char g_llm_host[128]     = {0};
static uint16_t g_llm_port      = 8080;
static char g_llm_path[256]     = "/v1/chat/completions";

/* ── Catalog helpers ─────────────────────────────────────────────────── */

static void catalog_add(const char *phash, const char *spec,
                        int prov, int template_id, int validated)
{
    CatalogEntry *e;
    if (g_catalog.count < MAX_CATALOG_ENTRIES) {
        e = &g_catalog.entries[g_catalog.count++];
    } else {
        /* Ring eviction */
        e = &g_catalog.entries[g_catalog.next % MAX_CATALOG_ENTRIES];
        g_catalog.next++;
    }
    strncpy(e->phash,  phash, sizeof(e->phash) - 1);
    strncpy(e->spec,   spec,  sizeof(e->spec) - 1);
    e->provenance   = prov;
    e->template_id  = template_id;
    e->generated_at = time(NULL);
    e->validated    = validated;
}

/* ── Simple JSON field extractor ─────────────────────────────────────── */

static int json_str(const char *json, const char *key,
                    char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (int)i;
}

/* ── LLM program generation ──────────────────────────────────────────── */

/*
 * Ask an OpenAI-compatible LLM to generate compact NML for the given spec.
 * Writes compact (pilcrow-delimited) NML to out_buf.
 * Returns chars written, or -1 on failure.
 */
static int llm_generate(const char *spec, char *out_buf, size_t out_sz)
{
    if (g_llm_host[0] == '\0') return -1;

    char req_body[2048];
    int body_len = snprintf(req_body, sizeof(req_body),
        "{\"model\":\"local\","
         "\"messages\":["
           "{\"role\":\"system\","
            "\"content\":\"Generate a minimal NML program for the given spec. "
            "Output only the program lines joined with the pilcrow character "
            "(\\u00b6), no explanation.\"},"
           "{\"role\":\"user\","
            "\"content\":\"%s\"}"
         "],"
         "\"max_tokens\":512}",
        spec);

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

    char *body = strstr(resp, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    char *p = strstr(body, "\"content\":");
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

/* ── Template generation ─────────────────────────────────────────────── */

static void fill_default_params(TemplateParams *p, int template_id)
{
    memset(p, 0, sizeof(*p));
    p->template_id    = template_id;
    p->input_dim      = 8;
    p->n_hidden       = 2;
    p->hidden_dims[0] = 16;
    p->hidden_dims[1] = 8;
    p->threshold      = 0.5f;
    p->output_classes = 3;
    p->epochs         = 100;
    p->lr_scaled      = 100;    /* 0.01 */
    strncpy(p->output_key, "score", sizeof(p->output_key) - 1);
}

static int generate_from_template(const char *intent,
                                  char *compact_out, size_t out_sz,
                                  int *template_id_out)
{
    int tid = template_select(intent);
    if (tid < 0) return -1;

    TemplateParams params;
    fill_default_params(&params, tid);

    int n = template_generate(&params, compact_out, out_sz);
    if (n <= 0) return -1;

    *template_id_out = tid;
    return n;
}

/* ── Validation ──────────────────────────────────────────────────────── */

/*
 * Validate compact NML by expanding and doing a dry-run with no data.
 * Returns 1 if the program assembles without error, 0 otherwise.
 */
static int validate_compact(const char *compact)
{
    char program_text[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact, program_text, sizeof(program_text)) < 0)
        return 0;

    char score_buf[32];
    int rc = nml_exec_run(program_text, NULL, score_buf, sizeof(score_buf));
    /* -1 = assembly/exec error; -2 = no score key (OK for validation) */
    return (rc != -1) ? 1 : 0;
}

/* ── Program submission ──────────────────────────────────────────────── */

/*
 * Encode compact NML as MSG_PROGRAM and publish to nml/program.
 * The Sentient will sign and re-broadcast to workers.
 * Returns the 16-char hex phash written to phash_out, or -1 on error.
 */
static int submit_program(const char *compact, char *phash_out)
{
    uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
    int pkt_len = msg_encode(pkt, sizeof(pkt), MSG_PROGRAM,
                             g_agent_name, g_http_port, compact);
    if (pkt_len <= 0) return -1;

    if (mqtt_transport_publish(&g_mqtt, MSG_PROGRAM, pkt, (size_t)pkt_len) < 0)
        return -1;

    /* Derive phash from payload for catalog (first 16 chars of compact as ID) */
    if (phash_out) {
        /* Use the first 16 printable chars of the compact as a rough ID */
        int j = 0;
        for (int i = 0; compact[i] && j < 16; i++) {
            unsigned char c = (unsigned char)compact[i];
            if (c >= 0x20 && c < 0x7f && c != 0xb6)
                phash_out[j++] = compact[i];
        }
        while (j < 16) phash_out[j++] = '0';
        phash_out[16] = '\0';
    }
    return 0;
}

/* ── Spec handler ────────────────────────────────────────────────────── */

static void handle_spec(const char *sender, const char *payload)
{
    /* Extract "spec" intent from JSON payload */
    char intent[256] = {0};
    if (json_str(payload, "spec", intent, sizeof(intent)) <= 0) {
        /* Treat raw payload as intent if JSON extraction fails */
        strncpy(intent, payload, sizeof(intent) - 1);
    }
    if (intent[0] == '\0') return;

    printf("[architect] spec from %s: %.80s\n", sender, intent);

    char compact[NML_MAX_PROGRAM_LEN + 1];
    int  template_id = -1;
    int  provenance  = PROV_TEMPLATE;
    int  generated   = 0;

    /* Primary path: template engine */
    if (generate_from_template(intent, compact, sizeof(compact),
                               &template_id) > 0) {
        generated = 1;
        printf("[architect] template match: %s\n", template_name(template_id));
    }

    /* Fallback: LLM */
    if (!generated && g_llm_host[0] != '\0') {
        if (llm_generate(intent, compact, sizeof(compact)) > 0) {
            generated  = 1;
            provenance = PROV_LLM;
            printf("[architect] LLM generated program\n");
        }
    }

    if (!generated) {
        fprintf(stderr,
                "[architect] no template match and no LLM — skipping spec\n");
        return;
    }

    /* Validate by dry-run */
    int valid = validate_compact(compact);
    if (!valid) {
        fprintf(stderr, "[architect] dry-run validation FAILED — dropping\n");
        return;
    }

    /* Submit to nml/program for Sentient to sign + broadcast */
    char phash[17] = {0};
    if (submit_program(compact, phash) < 0) {
        fprintf(stderr, "[architect] submit failed\n");
        return;
    }

    catalog_add(phash, intent, provenance, template_id, valid);
    printf("[architect] submitted program phash=%.16s  prov=%s  valid=%d\n",
           phash, provenance == PROV_TEMPLATE ? "template" : "llm", valid);
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
             "\"peers\":%d,\"catalog\":%d,"
             "\"templates\":%d,\"llm\":%s}",
            g_agent_name, g_peers.count, g_catalog.count,
            TEMPLATE_COUNT, g_llm_host[0] ? "true" : "false");
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /catalog — list generated programs */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/catalog") == 0) {
        char body[HTTP_BUF_SZ];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < g_catalog.count; i++) {
            const CatalogEntry *e = &g_catalog.entries[i];
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"phash\":\"%s\","
                 "\"spec\":\"%.80s\","
                 "\"provenance\":\"%s\","
                 "\"template\":\"%s\","
                 "\"validated\":%s}",
                e->phash,
                e->spec,
                e->provenance == PROV_TEMPLATE ? "template" : "llm",
                e->template_id >= 0 ? template_name(e->template_id) : "n/a",
                e->validated ? "true" : "false");
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /templates — available template names */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/templates") == 0) {
        char body[1024];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < TEMPLATE_COUNT; i++) {
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                            "{\"id\":%d,\"name\":\"%s\"}", i, template_name(i));
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /*
     * POST /generate  body: {"spec": "..."}
     * Immediately generate and submit a program for the given spec.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/generate") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            close(cfd); return;
        }
        body_start += 4;
        char spec[256] = {0};
        if (json_str(body_start, "spec", spec, sizeof(spec)) <= 0) {
            http_send(cfd, 400, "{\"error\":\"spec required\"}");
            close(cfd); return;
        }
        handle_spec("http", spec);
        http_send(cfd, 200, "{\"ok\":true}");
        close(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send(cfd, 200, body);
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
        "          [--port HTTP_PORT]\n"
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
    printf("[architect] identity  machine=%s  node=%s\n",
           g_machine_hash_hex, g_node_id_hex);

    /* ── Init tables ── */
    peer_table_init(&g_peers);
    memset(&g_catalog, 0, sizeof(g_catalog));

    /* ── MQTT ── */
    if (mqtt_transport_init(&g_mqtt, g_broker_host, g_broker_port,
                             g_agent_name, g_http_port,
                             g_identity_payload) != 0) {
        fprintf(stderr, "[architect] failed to connect to broker %s:%u\n",
                g_broker_host, g_broker_port);
        return 1;
    }

    /* ── HTTP server ── */
    g_http_fd = http_listen(g_http_port);
    if (g_http_fd < 0) {
        fprintf(stderr, "[architect] failed to bind HTTP on port %u\n",
                g_http_port);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }

    printf("[architect] HTTP API on port %u\n", g_http_port);
    printf("[architect] templates=%d  broker=%s:%u%s\n",
           TEMPLATE_COUNT, g_broker_host, g_broker_port,
           g_llm_host[0] ? "  llm=enabled" : "");

    /* ── Main loop ── */
    time_t last_heartbeat = 0;

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

            if (type == MSG_SPEC)
                handle_spec(pname, payload);
            /* Other message types observed but not acted on */
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
    }

    printf("[architect] shutting down\n");
    mqtt_transport_close(&g_mqtt);
    if (g_http_fd >= 0) close(g_http_fd);
    return 0;
}
