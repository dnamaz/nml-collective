/*
 * custodian_agent.c — NML Collective Custodian role.
 *
 * Data engineer: accepts raw data via HTTP, transforms it into NebulaDisk
 * float32 tensors, submits them to Sentient for quarantine approval, promotes
 * approved objects into the local data pool, and serves stored objects to
 * Workers via GET /objects/<hash>.
 *
 * Usage:
 *   ./custodian_agent [--name NAME] [--broker HOST] [--broker-port PORT]
 *                     [--port HTTP_PORT] [--data-dir DIR]
 *                     [--sentient HOST] [--sentient-port PORT]
 *                     [--stale-after SECONDS]
 *
 * Inputs:  POST /ingest (raw JSON float array or CSV)
 *          nml/data/approve  (Sentient approval, plain JSON via MQTT)
 * Outputs: nml/data/ready    (notifies Workers that data is available)
 *          nml/data/stale    (staleness alerts)
 *          GET /objects/<hash>  (data serving)
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/mqtt_transport.h"
#include "../../edge/storage.h"

/* Direct MQTT publish for plain JSON topics */
#include "../../edge/mqtt/mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_DATA_ITEMS       256
#define MAX_FLOATS_PER_ITEM  8192    /* max tensor elements per object */
#define HTTP_BUF_SZ          65536
#define HEARTBEAT_INTERVAL_S 5
#define STALE_DEFAULT_S      3600    /* 1 hour default staleness threshold */
#define STALE_POLL_S         60      /* check for stale items every minute */
#define STALE_PEER_S         30

/* Data item status */
#define STATUS_PENDING   0
#define STATUS_APPROVED  1
#define STATUS_REJECTED  2

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    char   hash[17];        /* NebulaDisk content hash */
    char   name[64];        /* human-readable dataset name */
    char   author[64];      /* submitting agent/source */
    int    n_samples;       /* number of float32 values */
    int    status;          /* STATUS_* */
    time_t submitted_at;
    time_t approved_at;
    int    served_count;    /* times this object has been requested */
} DataItem;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static MQTTTransport g_mqtt;
static PeerTable     g_peers;

static DataItem g_items[MAX_DATA_ITEMS];
static int      g_item_count = 0;

static const char *g_agent_name    = "custodian";
static char g_broker_host[128]     = "127.0.0.1";
static uint16_t g_broker_port      = 1883;
static uint16_t g_http_port        = 9004;
static char g_data_dir[256]        = "./.custodian-data";
static char g_sentient_host[128]   = "127.0.0.1";
static uint16_t g_sentient_port    = 9001;
static time_t g_stale_after        = STALE_DEFAULT_S;

/* ── JSON helpers ────────────────────────────────────────────────────── */

static int json_str(const char *json, const char *key,
                    char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
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

/* ── Data transformation ─────────────────────────────────────────────── */

/*
 * Parse a JSON array (or comma-separated values) of numbers into float32.
 * Handles both "[1.0, 2.0, 3.0]" and "1.0,2.0,3.0" formats.
 * Returns number of floats parsed, or -1 on error.
 */
static int parse_floats(const char *src, float *out, int max_count)
{
    if (!src || !out || max_count <= 0) return -1;

    const char *p = src;
    /* Skip leading whitespace and opening bracket */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '[') p++;

    int count = 0;
    char *end;
    while (*p && *p != ']' && count < max_count) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p == ',') { p++; continue; }
        float v = strtof(p, &end);
        if (end == p) break;   /* no conversion */
        out[count++] = v;
        p = end;
    }
    return count;
}

/*
 * Parse CSV rows: each row is a line, values are comma-separated floats.
 * All rows are flattened into a single float32 array.
 * Returns total floats parsed.
 */
static int parse_csv(const char *src, float *out, int max_count)
{
    if (!src || !out || max_count <= 0) return -1;
    int total = 0;
    const char *line = src;
    while (*line && total < max_count) {
        /* Find end of line */
        const char *eol = strchr(line, '\n');
        char row[1024];
        size_t row_len = eol ? (size_t)(eol - line) : strlen(line);
        if (row_len >= sizeof(row)) row_len = sizeof(row) - 1;
        memcpy(row, line, row_len);
        row[row_len] = '\0';

        /* Skip empty / comment lines */
        if (row[0] == '\0' || row[0] == '#') {
            line = eol ? eol + 1 : line + row_len;
            continue;
        }

        int n = parse_floats(row, out + total, max_count - total);
        if (n > 0) total += n;
        line = eol ? eol + 1 : line + row_len;
        if (!eol) break;
    }
    return total;
}

/* ── Internal HTTP client ────────────────────────────────────────────── */

static int http_post(const char *host, uint16_t port,
                     const char *path, const char *body,
                     char *resp_buf, size_t resp_sz)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }

    char req[4096];
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        path, host, port, strlen(body), body);
    write(fd, req, (size_t)req_len);

    char raw[HTTP_BUF_SZ];
    ssize_t rn = read(fd, raw, sizeof(raw) - 1);
    close(fd);
    if (rn <= 0) return -1;
    raw[rn] = '\0';
    char *hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end) return -1;
    hdr_end += 4;
    size_t bl = (size_t)(rn - (hdr_end - raw));
    if (bl >= resp_sz) bl = resp_sz - 1;
    memcpy(resp_buf, hdr_end, bl);
    resp_buf[bl] = '\0';
    return (int)bl;
}

/* ── Data store directory init ───────────────────────────────────────── */

static void ensure_data_dir(void)
{
    mkdir(g_data_dir, 0755);
    char obj_dir[280];
    snprintf(obj_dir, sizeof(obj_dir), "%s/objects", g_data_dir);
    mkdir(obj_dir, 0755);
}

/* ── DataItem helpers ────────────────────────────────────────────────── */

static DataItem *item_find(const char *hash)
{
    for (int i = 0; i < g_item_count; i++) {
        if (strcmp(g_items[i].hash, hash) == 0)
            return &g_items[i];
    }
    return NULL;
}

static DataItem *item_add(const char *hash, const char *name,
                          const char *author, int n_samples)
{
    if (g_item_count >= MAX_DATA_ITEMS) return NULL;
    DataItem *it = &g_items[g_item_count++];
    memset(it, 0, sizeof(*it));
    strncpy(it->hash,   hash,   sizeof(it->hash) - 1);
    strncpy(it->name,   name,   sizeof(it->name) - 1);
    strncpy(it->author, author, sizeof(it->author) - 1);
    it->n_samples    = n_samples;
    it->status       = STATUS_PENDING;
    it->submitted_at = time(NULL);
    return it;
}

/* ── Approval + ready notification ──────────────────────────────────── */

static void on_approved(const char *hash)
{
    DataItem *it = item_find(hash);
    if (!it) return;
    if (it->status == STATUS_APPROVED) return;

    it->status      = STATUS_APPROVED;
    it->approved_at = time(NULL);

    /* Publish nml/data/ready so Workers know this object is available */
    char json[256];
    int n = snprintf(json, sizeof(json),
        "{\"hash\":\"%s\",\"name\":\"%s\","
         "\"n_samples\":%d,\"port\":%u}",
        it->hash, it->name, it->n_samples, g_http_port);
    mqtt_publish(&g_mqtt.client, "nml/data/ready",
                 (const uint8_t *)json, (size_t)n,
                 MQTT_PUBLISH_QOS_1);
    mqtt_sync(&g_mqtt.client);
    printf("[custodian] data ready  hash=%s  name=%s  samples=%d\n",
           it->hash, it->name, it->n_samples);
}

static void on_rejected(const char *hash)
{
    DataItem *it = item_find(hash);
    if (it) it->status = STATUS_REJECTED;
}

/* ── Staleness monitoring ────────────────────────────────────────────── */

static void check_staleness(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_item_count; i++) {
        DataItem *it = &g_items[i];
        if (it->status != STATUS_APPROVED) continue;
        time_t age = now - it->approved_at;
        if (age < g_stale_after) continue;

        char json[256];
        int n = snprintf(json, sizeof(json),
            "{\"hash\":\"%s\",\"name\":\"%s\","
             "\"age_seconds\":%ld}",
            it->hash, it->name, (long)age);
        mqtt_publish(&g_mqtt.client, "nml/data/stale",
                     (const uint8_t *)json, (size_t)n,
                     MQTT_PUBLISH_QOS_1);
        mqtt_sync(&g_mqtt.client);
        printf("[custodian] stale data  hash=%s  age=%lds\n",
               it->hash, (long)age);

        /* Reset timer to avoid repeated alerts */
        it->approved_at = now;
    }
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
        code, code == 200 ? "OK" : code == 201 ? "Created" : "Error",
        strlen(body));
    write(fd, hdr, (size_t)hdr_len);
    write(fd, body, strlen(body));
}

static void http_send_binary(int fd, const char *content_type,
                              const char *data, size_t len)
{
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        content_type, len);
    write(fd, hdr, (size_t)hdr_len);
    write(fd, data, len);
}

static void handle_http(int cfd)
{
    char req[HTTP_BUF_SZ];
    ssize_t n = read(cfd, req, sizeof(req) - 1);
    if (n <= 0) { close(cfd); return; }
    req[n] = '\0';

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        close(cfd); return;
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        int approved = 0;
        for (int i = 0; i < g_item_count; i++)
            if (g_items[i].status == STATUS_APPROVED) approved++;
        char body[256];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"items\":%d,\"approved\":%d}",
            g_agent_name, g_item_count, approved);
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /pool — list all data items */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/pool") == 0) {
        char body[HTTP_BUF_SZ / 2];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < g_item_count; i++) {
            const DataItem *it = &g_items[i];
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"hash\":\"%s\",\"name\":\"%s\","
                 "\"author\":\"%s\",\"n_samples\":%d,"
                 "\"status\":\"%s\",\"served\":%d}",
                it->hash, it->name, it->author, it->n_samples,
                it->status == STATUS_APPROVED ? "approved"
                : it->status == STATUS_REJECTED ? "rejected" : "pending",
                it->served_count);
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        close(cfd); return;
    }

    /* GET /objects/<hash> — serve stored NebulaDisk object to Workers */
    if (strcmp(method, "GET") == 0 &&
        strncmp(path, "/objects/", 9) == 0) {
        const char *hash = path + 9;
        /* Remove trailing query string if present */
        char clean_hash[17] = {0};
        int j = 0;
        while (hash[j] && hash[j] != '?' && j < 16) {
            clean_hash[j] = hash[j]; j++;
        }
        clean_hash[j] = '\0';

        if (!storage_exists(g_data_dir, clean_hash)) {
            http_send(cfd, 404, "{\"error\":\"not found\"}");
            close(cfd); return;
        }

        char obj_path[512];
        if (storage_path(g_data_dir, clean_hash,
                         obj_path, sizeof(obj_path)) < 0) {
            http_send(cfd, 500, "{\"error\":\"storage error\"}");
            close(cfd); return;
        }

        /* Stream the raw .obj file */
        FILE *f = fopen(obj_path, "rb");
        if (!f) {
            http_send(cfd, 500, "{\"error\":\"read error\"}");
            close(cfd); return;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *file_buf = malloc((size_t)fsz);
        if (!file_buf) {
            fclose(f);
            http_send(cfd, 500, "{\"error\":\"oom\"}");
            close(cfd); return;
        }
        size_t rd = fread(file_buf, 1, (size_t)fsz, f);
        fclose(f);

        /* Update served count */
        DataItem *it = item_find(clean_hash);
        if (it) it->served_count++;

        http_send_binary(cfd, "application/octet-stream", file_buf, rd);
        free(file_buf);
        close(cfd); return;
    }

    /*
     * POST /ingest  — accept raw data and transform into NebulaDisk tensor.
     *
     * Body (JSON):
     *   {"name": "dataset-label", "data": [1.0, 2.0, ...], "format": "json|csv"}
     *
     * The "data" value may be:
     *   - A JSON array:  [1.0, 2.0, ...]  (format=json or omitted)
     *   - A CSV string:  "1.0,2.0\n3.0,4.0"  (format=csv)
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/ingest") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            close(cfd); return;
        }
        body_start += 4;

        char name[64]   = "unnamed";
        char format[16] = "json";
        json_str(body_start, "name",   name,   sizeof(name));
        json_str(body_start, "format", format, sizeof(format));

        /* Find the "data" field value */
        const char *data_start = strstr(body_start, "\"data\":");
        if (!data_start) {
            http_send(cfd, 400, "{\"error\":\"data field required\"}");
            close(cfd); return;
        }
        data_start += 7;
        while (*data_start == ' ') data_start++;

        /* Parse floats */
        static float floats[MAX_FLOATS_PER_ITEM];
        int n_floats = 0;
        if (strcmp(format, "csv") == 0) {
            /* data value is a quoted CSV string */
            if (*data_start == '"') data_start++;
            n_floats = parse_csv(data_start, floats, MAX_FLOATS_PER_ITEM);
        } else {
            n_floats = parse_floats(data_start, floats, MAX_FLOATS_PER_ITEM);
        }

        if (n_floats <= 0) {
            http_send(cfd, 400, "{\"error\":\"no numeric data found\"}");
            close(cfd); return;
        }

        /* Store as NebulaDisk object */
        char hash[17];
        if (storage_put(g_data_dir, (const char *)floats,
                        (size_t)n_floats * sizeof(float),
                        STORAGE_OBJ_DATA, g_agent_name, name, hash) < 0) {
            http_send(cfd, 500, "{\"error\":\"storage failed\"}");
            close(cfd); return;
        }

        /* Track in DataItem list */
        DataItem *it = item_find(hash);
        if (!it) {
            it = item_add(hash, name, g_agent_name, n_floats);
        }

        /* Forward to Sentient quarantine */
        char sentient_body[512];
        snprintf(sentient_body, sizeof(sentient_body),
            "{\"name\":\"%s\",\"hash\":\"%s\","
             "\"n_samples\":%d,\"author\":\"%s\"}",
            name, hash, n_floats, g_agent_name);
        char sentient_resp[512];
        http_post(g_sentient_host, g_sentient_port,
                  "/data/submit", sentient_body,
                  sentient_resp, sizeof(sentient_resp));

        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"hash\":\"%s\",\"n_samples\":%d,\"status\":\"pending\"}",
            hash, n_floats);
        http_send(cfd, 201, resp);

        printf("[custodian] ingested '%s'  samples=%d  hash=%s\n",
               name, n_floats, hash);
        close(cfd); return;
    }

    /*
     * POST /approve  body: {"hash": "<16hex>"}
     * Manually approve a pending item (for testing or admin use).
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/approve") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) { http_send(cfd, 400, "{\"error\":\"no body\"}"); close(cfd); return; }
        body_start += 4;
        char hash[17] = {0};
        json_str(body_start, "hash", hash, sizeof(hash));
        if (hash[0] == '\0') {
            http_send(cfd, 400, "{\"error\":\"hash required\"}"); close(cfd); return;
        }
        on_approved(hash);
        http_send(cfd, 200, "{\"ok\":true}");
        close(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ / 4];
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
        "          [--port HTTP_PORT] [--data-dir DIR]\n"
        "          [--sentient HOST] [--sentient-port PORT]\n"
        "          [--stale-after SECONDS]\n",
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
        else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc)
            strncpy(g_data_dir, argv[++i], sizeof(g_data_dir) - 1);
        else if (strcmp(argv[i], "--sentient") == 0 && i + 1 < argc)
            strncpy(g_sentient_host, argv[++i], sizeof(g_sentient_host) - 1);
        else if (strcmp(argv[i], "--sentient-port") == 0 && i + 1 < argc)
            g_sentient_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--stale-after") == 0 && i + 1 < argc)
            g_stale_after = (time_t)atoi(argv[++i]);
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
    printf("[custodian] identity  machine=%s  node=%s\n",
           g_machine_hash_hex, g_node_id_hex);

    /* ── Storage ── */
    ensure_data_dir();

    /* ── Init tables ── */
    peer_table_init(&g_peers);
    memset(g_items, 0, sizeof(g_items));

    /* ── MQTT ── */
    if (mqtt_transport_init(&g_mqtt, g_broker_host, g_broker_port,
                             g_agent_name, g_http_port,
                             g_identity_payload) != 0) {
        fprintf(stderr, "[custodian] failed to connect to broker %s:%u\n",
                g_broker_host, g_broker_port);
        return 1;
    }

    /* ── HTTP server ── */
    g_http_fd = http_listen(g_http_port);
    if (g_http_fd < 0) {
        fprintf(stderr, "[custodian] failed to bind HTTP on port %u\n",
                g_http_port);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }

    printf("[custodian] HTTP API on port %u\n", g_http_port);
    printf("[custodian] data-dir=%s  sentient=%s:%u  stale-after=%lds\n",
           g_data_dir, g_sentient_host, g_sentient_port,
           (long)g_stale_after);

    /* ── Main loop ── */
    time_t last_heartbeat = 0;
    time_t last_stale     = 0;

    while (g_running) {
        time_t now = time(NULL);

        /* Wait up to 1 second for HTTP connections */
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
        }

        /* Heartbeat */
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

        /* Staleness check */
        if (now - last_stale >= STALE_POLL_S) {
            check_staleness();
            last_stale = now;
        }
    }

    printf("[custodian] shutting down\n");
    mqtt_transport_close(&g_mqtt);
    if (g_http_fd >= 0) close(g_http_fd);
    return 0;
}
