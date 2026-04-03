/*
 * sentient_agent.c — NML Collective Sentient role.
 *
 * The collective's trust anchor and consensus finalizer.
 * Signs NML programs with Ed25519, broadcasts them to workers, collects
 * execution results, finalizes consensus, and serves data over HTTP.
 *
 * Usage:
 *   ./sentient_agent [--name NAME] [--broker HOST] [--broker-port PORT]
 *                    [--port HTTP_PORT] [--key KEY_HEX_OR_FILE]
 *                    [--key-agent NAME] [--data-dir DIR]
 *                    [--herald HOST] [--herald-port PORT] [--quorum N]
 *
 * Key format: "ed25519:<128 hex chars>"  (same as nml-crypto --keygen output)
 * Key file:   path to a file containing the key on the first line
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/crypto.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/vote.h"
#include "../../edge/mqtt_transport.h"
#include "../../edge/storage.h"
#include "../../edge/http_client.h"

/* For direct mqtt_publish on custom topics (nml/committed/<phash>) */
#include "../../edge/mqtt/mqtt.h"

#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

/* Pull in nml_sign_program */
#define NML_CRYPTO
#include "../../nml/runtime/nml_crypto.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_QUARANTINE    64
#define MAX_DATA_NAMES    64
#define HTTP_BUF_SZ       65536    /* large enough for object bodies */
#define JSON_VAL_SZ       256

#define QUARANTINE_PENDING  0
#define QUARANTINE_APPROVED 1
#define QUARANTINE_REJECTED 2

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    char hash[17];
    char name[64];
    char author[64];
    int  status;
    char reason[128];
    time_t submitted_at;
} QuarantineEntry;

typedef struct {
    char name[64];
    char hash[17];   /* latest approved hash */
} DataNameEntry;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static MQTTTransport g_mqtt;
static PeerTable     g_peers;
static VoteTable     g_votes;

static QuarantineEntry g_quarantine[MAX_QUARANTINE];
static int             g_quarantine_count = 0;

static DataNameEntry   g_data_names[MAX_DATA_NAMES];
static int             g_data_names_count = 0;

static const char *g_agent_name  = "sentient";
static char g_broker_host[128]   = "127.0.0.1";
static uint16_t g_broker_port    = 1883;
static uint16_t g_http_port      = 9001;
static char g_key_hex[512]       = {0};
static char g_key_agent[64]      = {0};
static char g_data_dir[256]      = "./.sentient-data";
static char g_herald_host[128]   = "127.0.0.1";
static uint16_t g_herald_port    = 7780;
static int g_quorum              = 1;

/* ── HTTP server helpers ─────────────────────────────────────────────── */

static int json_str(const char *json, const char *key,
                    char *out_buf, size_t out_sz)
{
    if (!json || !key || !out_buf || out_sz == 0) {
        return -1;
    }

    char needle[JSON_VAL_SZ];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *p = strstr(json, needle);
    if (!p) {
        return -1;
    }
    p += strlen(needle);

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') {
        return -1;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    if (*p != '"') {
        return -1;
    }
    p++;

    size_t written = 0;
    while (*p && *p != '"') {
        if (written < out_sz - 1) {
            out_buf[written++] = *p;
        }
        p++;
    }
    out_buf[written] = '\0';

    if (*p != '"') {
        return -1;
    }
    return 0;
}

static void http_respond(compat_socket_t fd, int status, const char *body)
{
    const char *status_str;
    switch (status) {
    case 200: status_str = "OK";                    break;
    case 400: status_str = "Bad Request";           break;
    case 404: status_str = "Not Found";             break;
    case 405: status_str = "Method Not Allowed";    break;
    default:  status_str = "Internal Server Error"; break;
    }

    size_t body_len = body ? strlen(body) : 0;

    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.0 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, status_str, body_len);

    send(fd, header, strlen(header), 0);
    if (body && body_len > 0) {
        send(fd, body, body_len, 0);
    }
}

/* ── Core collective functions ───────────────────────────────────────── */

static int sign_and_broadcast(const char *program_text)
{
    static char signed_buf[NML_MAX_PROGRAM_LEN + 512];
    const char *source = program_text;

    if (g_key_hex[0]) {
        const char *key_agent = g_key_agent[0] ? g_key_agent : g_agent_name;
        int rc = nml_sign_program(program_text, key_agent,
                                  g_key_hex, signed_buf, sizeof(signed_buf));
        if (rc < 0) {
            fprintf(stderr, "[%s] signing failed\n", g_agent_name);
            return -1;
        }
        source = signed_buf;
    } else {
        fprintf(stderr, "[%s] WARNING: broadcasting unsigned program (no key)\n",
                g_agent_name);
    }

    static char compact[NML_MAX_PROGRAM_LEN + 1];
    if (msg_program_to_compact(source, compact, sizeof(compact)) < 0) {
        fprintf(stderr, "[%s] compact encode overflow\n", g_agent_name);
        return -1;
    }

    static uint8_t wire[NML_MAX_PROGRAM_LEN + 256];
    int n = msg_encode(wire, sizeof(wire),
                       MSG_PROGRAM, g_agent_name, g_http_port, compact);
    if (n < 0) return -1;

    /* Compute and log the program hash */
    char phash[17];
    crypto_program_hash(source, phash);
    printf("[%s] broadcasting %s (signed=%s)\n",
           g_agent_name, phash, g_key_hex[0] ? "yes" : "no");

    return mqtt_transport_publish(&g_mqtt, MSG_PROGRAM, wire, (size_t)n);
}

static void commit_result(const char *phash, float score, int vote_count)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "nml/committed/%s", phash);

    char body[256];
    int blen = snprintf(body, sizeof(body),
                        "{\"phash\":\"%s\",\"score\":%.6f,\"votes\":%d}",
                        phash, score, vote_count);

    mqtt_publish(&g_mqtt.client, topic,
                 (const uint8_t *)body, (size_t)blen,
                 MQTT_PUBLISH_QOS_1);
    mqtt_sync(&g_mqtt.client);

    printf("[%s] COMMITTED %s score=%.6f votes=%d\n",
           g_agent_name, phash, score, vote_count);
}

static void handle_result(const char *peer_name, const char *payload)
{
    char phash[17] = {0};
    float score = 0.0f;
    if (sscanf(payload, "%16[^:]:%f", phash, &score) != 2) return;

    int r = vote_add(&g_votes, phash, peer_name, score, g_quorum, time(NULL));
    if (r == 1) {
        float mean;
        if (vote_get_result(&g_votes, phash, &mean) == 0) {
            commit_result(phash, mean, g_quorum);
        }
    }
}

/* ── HTTP request handlers ───────────────────────────────────────────── */

static void handle_health(compat_socket_t fd)
{
    /* count pending quarantine entries */
    int pending = 0;
    for (int i = 0; i < g_quarantine_count; i++) {
        if (g_quarantine[i].status == QUARANTINE_PENDING) pending++;
    }

    char body[512];
    snprintf(body, sizeof(body),
             "{\"status\":\"healthy\",\"name\":\"%s\","
             "\"broker\":\"%s:%d\",\"quorum\":%d,"
             "\"quarantine_pending\":%d,\"data_names\":%d}",
             g_agent_name,
             g_broker_host, (int)g_broker_port,
             g_quorum,
             pending,
             g_data_names_count);
    http_respond(fd, 200, body);
}

static void handle_get_object(compat_socket_t fd, const char *hash)
{
    if (!hash || strlen(hash) != 16) {
        http_respond(fd, 400, "{\"error\":\"invalid hash\"}");
        return;
    }

    char fpath[512];
    if (storage_path(g_data_dir, hash, fpath, sizeof(fpath)) < 0) {
        http_respond(fd, 404, "{\"error\":\"not found\"}");
        return;
    }

    FILE *f = fopen(fpath, "rb");
    if (!f) {
        http_respond(fd, 404, "{\"error\":\"not found\"}");
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    rewind(f);

    /* Write header */
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.0 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Length: %ld\r\n"
                        "Connection: close\r\n\r\n", file_sz);
    send(fd, hdr, hlen, 0);

    /* Stream file content */
    uint8_t chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        send(fd, (const char *)chunk, n, 0);
    }
    fclose(f);
}

static void handle_data_get(compat_socket_t fd, const char *query)
{
    if (!query) {
        http_respond(fd, 400, "{\"error\":\"missing query\"}");
        return;
    }

    /* Extract name= from query string */
    const char *name_start = strstr(query, "name=");
    if (!name_start) {
        http_respond(fd, 400, "{\"error\":\"missing name parameter\"}");
        return;
    }
    name_start += 5; /* skip "name=" */

    char name[64] = {0};
    size_t ni = 0;
    while (*name_start && *name_start != '&' && ni < sizeof(name) - 1) {
        name[ni++] = *name_start++;
    }
    name[ni] = '\0';

    /* Look up in g_data_names[] */
    for (int i = 0; i < g_data_names_count; i++) {
        if (strcmp(g_data_names[i].name, name) == 0) {
            char body[256];
            snprintf(body, sizeof(body),
                     "{\"hash\":\"%s\",\"name\":\"%s\","
                     "\"object_url\":\"/objects/%s\"}",
                     g_data_names[i].hash,
                     g_data_names[i].name,
                     g_data_names[i].hash);
            http_respond(fd, 200, body);
            return;
        }
    }

    http_respond(fd, 404, "{\"error\":\"not found\"}");
}

static void handle_data_submit(compat_socket_t fd, const char *body)
{
    char name[64]    = {0};
    char content[NML_MAX_PROGRAM_LEN + 1] = {0};
    char author[64]  = {0};

    json_str(body, "name",    name,    sizeof(name));
    json_str(body, "content", content, sizeof(content));
    json_str(body, "author",  author,  sizeof(author));

    if (content[0] == '\0') {
        http_respond(fd, 400, "{\"error\":\"content required\"}");
        return;
    }

    if (g_quarantine_count >= MAX_QUARANTINE) {
        http_respond(fd, 400, "{\"error\":\"quarantine full\"}");
        return;
    }

    char hash_out[17] = {0};
    if (storage_put(g_data_dir, content, strlen(content),
                    STORAGE_OBJ_DATA, author, name, hash_out) < 0) {
        http_respond(fd, 500, "{\"error\":\"storage failed\"}");
        return;
    }

    /* Add to quarantine */
    QuarantineEntry *qe = &g_quarantine[g_quarantine_count++];
    snprintf(qe->hash,   sizeof(qe->hash),   "%s", hash_out);
    snprintf(qe->name,   sizeof(qe->name),   "%s", name);
    snprintf(qe->author, sizeof(qe->author), "%s", author);
    qe->status       = QUARANTINE_PENDING;
    qe->reason[0]    = '\0';
    qe->submitted_at = time(NULL);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"hash\":\"%s\",\"status\":\"pending\","
             "\"object_url\":\"/objects/%s\"}",
             hash_out, hash_out);
    http_respond(fd, 200, resp);
}

static void handle_data_approve(compat_socket_t fd, const char *body)
{
    char hash[17] = {0};
    if (json_str(body, "hash", hash, sizeof(hash)) < 0) {
        http_respond(fd, 400, "{\"error\":\"hash required\"}");
        return;
    }

    /* Find in quarantine */
    int found = -1;
    for (int i = 0; i < g_quarantine_count; i++) {
        if (strcmp(g_quarantine[i].hash, hash) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        http_respond(fd, 404, "{\"error\":\"hash not in quarantine\"}");
        return;
    }

    g_quarantine[found].status = QUARANTINE_APPROVED;

    /* Register in g_data_names[] — update or append name→hash mapping */
    const char *qname = g_quarantine[found].name;
    int name_idx = -1;
    for (int i = 0; i < g_data_names_count; i++) {
        if (strcmp(g_data_names[i].name, qname) == 0) {
            name_idx = i;
            break;
        }
    }
    if (name_idx < 0 && g_data_names_count < MAX_DATA_NAMES) {
        name_idx = g_data_names_count++;
    }
    if (name_idx >= 0) {
        snprintf(g_data_names[name_idx].name, sizeof(g_data_names[name_idx].name),
                 "%s", qname);
        snprintf(g_data_names[name_idx].hash, sizeof(g_data_names[name_idx].hash),
                 "%s", hash);
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"hash\":\"%s\",\"status\":\"approved\"}", hash);
    http_respond(fd, 200, resp);
}

static void handle_data_quarantine(compat_socket_t fd)
{
    static char buf[16384];
    int pos = 0;
    int sz = (int)sizeof(buf);

    pos += snprintf(buf + pos, (size_t)(sz - pos), "[");

    int first = 1;
    for (int i = 0; i < g_quarantine_count; i++) {
        if (g_quarantine[i].status != QUARANTINE_PENDING) continue;
        if (!first) {
            pos += snprintf(buf + pos, (size_t)(sz - pos), ",");
        }
        first = 0;
        pos += snprintf(buf + pos, (size_t)(sz - pos),
                        "{\"hash\":\"%s\",\"name\":\"%s\","
                        "\"author\":\"%s\",\"submitted_at\":%ld}",
                        g_quarantine[i].hash,
                        g_quarantine[i].name,
                        g_quarantine[i].author,
                        (long)g_quarantine[i].submitted_at);
        if (pos >= sz - 64) break; /* prevent overflow */
    }

    pos += snprintf(buf + pos, (size_t)(sz - pos), "]");
    http_respond(fd, 200, buf);
}

static void handle_data_pool(compat_socket_t fd)
{
    static char buf[8192];
    int pos = 0;
    int sz = (int)sizeof(buf);

    pos += snprintf(buf + pos, (size_t)(sz - pos), "[");

    for (int i = 0; i < g_data_names_count; i++) {
        if (i > 0) {
            pos += snprintf(buf + pos, (size_t)(sz - pos), ",");
        }
        pos += snprintf(buf + pos, (size_t)(sz - pos),
                        "{\"name\":\"%s\",\"hash\":\"%s\","
                        "\"object_url\":\"/objects/%s\"}",
                        g_data_names[i].name,
                        g_data_names[i].hash,
                        g_data_names[i].hash);
        if (pos >= sz - 128) break;
    }

    pos += snprintf(buf + pos, (size_t)(sz - pos), "]");
    http_respond(fd, 200, buf);
}

/* ── HTTP server ─────────────────────────────────────────────────────── */

static compat_socket_t make_server_socket(uint16_t port)
{
    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[%s] socket: %s\n", g_agent_name, strerror(errno));
        return COMPAT_INVALID_SOCKET;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, COMPAT_SOCKOPT_CAST(&opt), sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[%s] bind port %d: %s\n",
                g_agent_name, (int)port, strerror(errno));
        compat_close_socket(fd);
        return COMPAT_INVALID_SOCKET;
    }

    if (listen(fd, 8) < 0) {
        fprintf(stderr, "[%s] listen: %s\n", g_agent_name, strerror(errno));
        compat_close_socket(fd);
        return COMPAT_INVALID_SOCKET;
    }

    return fd;
}

static void http_serve_once(compat_socket_t server_fd)
{
    compat_socket_t client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == COMPAT_INVALID_SOCKET) {
        return;
    }

    static char buf[HTTP_BUF_SZ];
    int total = 0;
    int found_headers = 0;

    /* read until \r\n\r\n found or buffer full */
    while (total < HTTP_BUF_SZ - 1) {
        int n = (int)recv(client_fd, buf + total, (size_t)(HTTP_BUF_SZ - 1 - total), 0);
        if (n <= 0) {
            break;
        }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) {
            found_headers = 1;
            break;
        }
    }

    if (!found_headers) {
        compat_close_socket(client_fd);
        return;
    }

    buf[total] = '\0';

    /* parse method and path from first line */
    char method[16] = {0};
    char path[256]  = {0};
    sscanf(buf, "%15s %255s", method, path);

    /* find body pointer (after \r\n\r\n) */
    char *body_ptr = strstr(buf, "\r\n\r\n");
    if (body_ptr) {
        body_ptr += 4;
    } else {
        body_ptr = buf + total;
    }

    /* check Content-Length and read more body data if needed */
    char *cl_hdr = strstr(buf, "Content-Length:");
    if (!cl_hdr) {
        cl_hdr = strstr(buf, "content-length:");
    }
    if (cl_hdr) {
        int content_length = 0;
        sscanf(cl_hdr + 15, " %d", &content_length);

        int body_have = (int)(buf + total - body_ptr);
        while (body_have < content_length && total < HTTP_BUF_SZ - 1) {
            int n = (int)recv(client_fd, buf + total,
                              (size_t)(HTTP_BUF_SZ - 1 - total), 0);
            if (n <= 0) {
                break;
            }
            total += n;
            body_have += n;
        }
        buf[total] = '\0';
        body_ptr = strstr(buf, "\r\n\r\n");
        if (body_ptr) {
            body_ptr += 4;
        } else {
            body_ptr = buf + total;
        }
    }

    /* split path and query string */
    char path_only[256] = {0};
    char *query = NULL;
    {
        char *q = strchr(path, '?');
        if (q) {
            size_t plen = (size_t)(q - path);
            if (plen >= sizeof(path_only)) plen = sizeof(path_only) - 1;
            memcpy(path_only, path, plen);
            path_only[plen] = '\0';
            query = q + 1;
        } else {
            snprintf(path_only, sizeof(path_only), "%s", path);
        }
    }

    /* dispatch */
    if (strcmp(method, "GET") == 0 && strcmp(path_only, "/health") == 0) {
        handle_health(client_fd);

    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path_only, "/objects/", 9) == 0) {
        /* extract hash from path: /objects/<hash> */
        const char *hash = path_only + 9;
        handle_get_object(client_fd, hash);

    } else if (strcmp(method, "GET") == 0 &&
               strcmp(path_only, "/data/get") == 0) {
        handle_data_get(client_fd, query);

    } else if (strcmp(method, "POST") == 0 &&
               strcmp(path_only, "/data/submit") == 0) {
        handle_data_submit(client_fd, body_ptr);

    } else if (strcmp(method, "POST") == 0 &&
               strcmp(path_only, "/data/approve") == 0) {
        handle_data_approve(client_fd, body_ptr);

    } else if (strcmp(method, "GET") == 0 &&
               strcmp(path_only, "/data/quarantine") == 0) {
        handle_data_quarantine(client_fd);

    } else if (strcmp(method, "GET") == 0 &&
               strcmp(path_only, "/data/pool") == 0) {
        handle_data_pool(client_fd);

    } else if (strcmp(method, "OPTIONS") == 0) {
        http_respond(client_fd, 200, "{}");

    } else {
        http_respond(client_fd, 404, "{\"error\":\"not found\"}");
    }

    compat_close_socket(client_fd);
}

/* ── Herald credential issuance ──────────────────────────────────────── */

static void issue_credential(const char *agent_name, const char *password,
                              const char *role)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"agent\":\"%s\",\"password\":\"%s\",\"role\":\"%s\"}",
             agent_name, password, role);

    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COMPAT_INVALID_SOCKET) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(g_herald_port);
    addr.sin_addr.s_addr = inet_addr(g_herald_host);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        compat_close_socket(fd);
        return;
    }

    char req[512];
    int rlen = snprintf(req, sizeof(req),
                        "POST /credentials/issue HTTP/1.0\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "\r\n%s",
                        strlen(body), body);
    send(fd, req, rlen, 0);

    /* Read and discard response */
    char resp[512];
    recv(fd, resp, sizeof(resp) - 1, 0);
    compat_close_socket(fd);

    printf("[%s] issued credential to Herald for '%s' (role=%s)\n",
           g_agent_name, agent_name, role);
}

/* ── Key loading ─────────────────────────────────────────────────────── */

static void load_key(const char *key_arg)
{
    if (!key_arg || key_arg[0] == '\0') return;

    /* Treat as file path if starts with '/' or './' */
    if (key_arg[0] == '/' || (key_arg[0] == '.' && key_arg[1] == '/')) {
        FILE *f = fopen(key_arg, "r");
        if (!f) {
            fprintf(stderr, "[%s] cannot open key file: %s\n",
                    g_agent_name, key_arg);
            return;
        }
        char line[512] = {0};
        if (fgets(line, sizeof(line), f)) {
            /* strip trailing whitespace */
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                               line[len-1] == ' '  || line[len-1] == '\t')) {
                line[--len] = '\0';
            }
            snprintf(g_key_hex, sizeof(g_key_hex), "%s", line);
        }
        fclose(f);
    } else {
        snprintf(g_key_hex, sizeof(g_key_hex), "%s", key_arg);
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    compat_winsock_init();

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            g_agent_name = argv[++i];
        } else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc) {
            snprintf(g_broker_host, sizeof(g_broker_host), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc) {
            g_broker_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_http_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            load_key(argv[++i]);
        } else if (strcmp(argv[i], "--key-agent") == 0 && i + 1 < argc) {
            snprintf(g_key_agent, sizeof(g_key_agent), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            snprintf(g_data_dir, sizeof(g_data_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--herald") == 0 && i + 1 < argc) {
            snprintf(g_herald_host, sizeof(g_herald_host), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--herald-port") == 0 && i + 1 < argc) {
            g_herald_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--quorum") == 0 && i + 1 < argc) {
            g_quorum = atoi(argv[++i]);
        } else {
            fprintf(stderr,
                    "Usage: %s [--name NAME] [--broker HOST] [--broker-port PORT]\n"
                    "          [--port HTTP_PORT] [--key KEY_HEX_OR_FILE]\n"
                    "          [--key-agent NAME] [--data-dir DIR]\n"
                    "          [--herald HOST] [--herald-port PORT] [--quorum N]\n",
                    argv[0]);
            return 1;
        }
    }

    /* 1. Derive identity */
    identity_init(g_agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);

    /* 2. Init peer and vote tables */
    peer_table_init(&g_peers);
    vote_table_init(&g_votes);

    /* 3. Ensure data directory structure exists */
    if (compat_mkdir(g_data_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[%s] warning: mkdir %s: %s\n",
                g_agent_name, g_data_dir, strerror(errno));
    }
    {
        char objs_dir[320];
        snprintf(objs_dir, sizeof(objs_dir), "%s/objects", g_data_dir);
        if (compat_mkdir(objs_dir, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "[%s] warning: mkdir %s: %s\n",
                    g_agent_name, objs_dir, strerror(errno));
        }
    }

    /* 4. Signal handlers */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    /* 5. Connect to MQTT broker */
    if (mqtt_transport_init(&g_mqtt, g_broker_host, g_broker_port,
                             g_agent_name, g_http_port,
                             g_identity_payload) < 0) {
        fprintf(stderr, "[%s] MQTT init failed\n", g_agent_name);
        return 1;
    }

    /* 6. Bind HTTP server socket */
    compat_socket_t server_fd = make_server_socket(g_http_port);
    if (server_fd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[%s] HTTP server init failed\n", g_agent_name);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }

    /* Startup banner */
    printf("[%s] Sentient active — broker=%s:%d  http=%d  quorum=%d\n",
           g_agent_name, g_broker_host, (int)g_broker_port,
           (int)g_http_port, g_quorum);
    printf("[%s]   data dir: %s\n", g_agent_name, g_data_dir);
    if (g_key_hex[0]) {
        /* print first 24 chars of key */
        char key_preview[25] = {0};
        strncpy(key_preview, g_key_hex, 24);
        printf("[%s]   key: %s...\n", g_agent_name, key_preview);
    } else {
        printf("[%s]   key: none — programs will be unsigned\n", g_agent_name);
    }
    printf("[%s]   HTTP: GET /objects/<hash> | GET /data/get?name=X"
           " | POST /data/submit | GET /health\n", g_agent_name);

    /* suppress unused warning for issue_credential */
    (void)issue_credential;

    /* Main loop */
    time_t last_heartbeat = time(NULL);
    time_t last_sweep     = last_heartbeat;

    while (g_running) {
        /* 1. HTTP select (1s timeout shared with MQTT sync) */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(COMPAT_SELECT_NFDS(server_fd), &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(server_fd, &rfds))
            http_serve_once(server_fd);

        if (mqtt_transport_sync(&g_mqtt, 0) < 0) {
            fprintf(stderr, "[%s] MQTT connection lost\n", g_agent_name);
            break;
        }

        /* 2. Process all queued MQTT messages */
        static uint8_t in_buf[NML_MAX_PROGRAM_LEN + 256];
        static char    peer_name[64];
        static char    payload[NML_MAX_PROGRAM_LEN + 1];

        for (;;) {
            int received = mqtt_transport_recv(&g_mqtt, in_buf, sizeof(in_buf),
                                               NULL);
            if (received <= 0) break;

            int msg_type;
            uint16_t peer_port;
            if (msg_parse(in_buf, (size_t)received,
                          &msg_type, peer_name, sizeof(peer_name),
                          &peer_port, payload, sizeof(payload)) < 0)
                continue;

            /* Ignore self */
            if (strcmp(peer_name, g_agent_name) == 0) continue;

            switch (msg_type) {
            case MSG_ANNOUNCE:
                peer_upsert(&g_peers, peer_name, "", peer_port,
                             payload, time(NULL));
                printf("[%s] peer: %s\n", g_agent_name, peer_name);
                break;

            case MSG_HEARTBEAT:
                peer_upsert(&g_peers, peer_name, "", peer_port,
                             payload, time(NULL));
                break;

            case MSG_PROGRAM: {
                /* From Architect (or another agent) — sign and broadcast to workers.
                   Skip if from another sentient to avoid re-signing loops. */
                if (strncmp(peer_name, "sentient", 8) == 0) break;

                /* Decode compact → full program */
                static char program_text[NML_MAX_PROGRAM_LEN + 1];
                if (msg_compact_to_program(payload, program_text,
                                           sizeof(program_text)) < 0)
                    break;
                printf("[%s] received program from %s — signing and broadcasting\n",
                       g_agent_name, peer_name);
                sign_and_broadcast(program_text);
                break;
            }

            case MSG_RESULT:
                handle_result(peer_name, payload);
                break;

            case MSG_ENFORCE: {
                char type_str[8] = {0};
                char target[64]  = {0};
                const char *p = payload;
                size_t tl = strcspn(p, "|");
                if (tl < sizeof(type_str)) { memcpy(type_str, p, tl); p += tl; }
                if (*p == '|') p++;
                size_t nl = strcspn(p, "|");
                if (nl < sizeof(target)) { memcpy(target, p, nl); }
                if (strncmp(type_str, "Q", 1) == 0) {
                    peer_quarantine(&g_peers, target, "enforced");
                } else if (strncmp(type_str, "U", 1) == 0) {
                    PeerEntry *pe = peer_get(&g_peers, target);
                    if (pe) { pe->quarantined = 0; pe->active = 1; }
                }
                break;
            }

            default: break;
            }
        }

        /* 3. Periodic heartbeat */
        time_t now = time(NULL);
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL) {
            static uint8_t hb_buf[256];
            int n = msg_encode(hb_buf, sizeof(hb_buf),
                               MSG_HEARTBEAT, g_agent_name, g_http_port,
                               g_identity_payload);
            if (n > 0)
                mqtt_transport_publish(&g_mqtt, MSG_HEARTBEAT, hb_buf,
                                       (size_t)n);
            last_heartbeat = now;
        }

        /* 4. Periodic sweep */
        if (now - last_sweep >= 30) {
            peer_sweep(&g_peers, now, HEARTBEAT_INTERVAL * 6);
            vote_expire(&g_votes, now, 120);
            last_sweep = now;
        }
    }

    printf("[%s] shutting down\n", g_agent_name);
    compat_close_socket(server_fd);
    mqtt_transport_close(&g_mqtt);
    compat_winsock_cleanup();
    return 0;
}
