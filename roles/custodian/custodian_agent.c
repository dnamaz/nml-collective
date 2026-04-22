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
 *          nml/data/reject   (Sentient rejection, plain JSON via MQTT)
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
#include "../../edge/chain.h"
#include "../../edge/http_util.h"

/* Embedded landing page, generated from ui.html via `xxd -i`. */
#include "ui.html.h"

/* Preserve existing call-site names; shared helpers live in http_util. */
#define http_send  http_send_json
#define json_str   http_json_str

/* Direct MQTT publish for plain JSON topics */
#include "../../edge/mqtt/mqtt.h"

#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#ifndef COMPAT_WINDOWS
#include <dirent.h>
#endif

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_DATA_ITEMS        256
#define SHARD_FLOAT_COUNT     8192   /* floats per shard */
#define MANIFEST_MAX_SHARDS   1024   /* max shards per manifest (= 8M floats) */
#define HTTP_BUF_SZ           (256 * 1024)  /* large enough for max float payload */
#define HTTP_MAX_BODY_SZ      (4 * 1024 * 1024)
#define HEARTBEAT_INTERVAL_S 5
#define STALE_DEFAULT_S      3600    /* 1 hour default staleness threshold */
#define STALE_POLL_S         60      /* check for stale items every minute */
#define STALE_PEER_S         30

/* Data item status.  Note: Windows <winnt.h> defines ITEM_PENDING, so we
 * use an ITEM_* prefix to avoid the collision. */
#define ITEM_PENDING   0
#define ITEM_APPROVED  1
#define ITEM_REJECTED  2
#define ITEM_SHARD     3  /* internal shard — approved alongside its manifest */

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    char   hash[17];          /* NebulaDisk content hash */
    char   name[64];          /* human-readable dataset name */
    char   author[64];        /* submitting agent/source */
    int    n_samples;         /* number of float32 values */
    int    status;            /* STATUS_* */
    time_t submitted_at;
    time_t approved_at;
    time_t last_stale_alert;  /* last time a stale alert was sent */
    int    served_count;      /* times this object has been requested */
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
static Chain         g_chain;

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

/* JSON string extraction lives in edge/http_util.c (see #define above). */

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

    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COMPAT_INVALID_SOCKET) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        compat_close_socket(fd); return -1;
    }

    char req[4096];
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        path, host, port, strlen(body), body);
    send(fd, req, (size_t)req_len, 0);

    char raw[HTTP_BUF_SZ];
    int rn = recv(fd, raw, sizeof(raw) - 1, 0);
    compat_close_socket(fd);
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
    compat_mkdir(g_data_dir, 0755);
    char obj_dir[280];
    snprintf(obj_dir, sizeof(obj_dir), "%s/objects", g_data_dir);
    compat_mkdir(obj_dir, 0755);
}

/* ── Sidecar .meta persistence ───────────────────────────────────────── */

/*
 * Write (or overwrite) {data_dir}/objects/{hash[0:2]}/{hash}.meta with the
 * fields that are not stored inside the NebulaDisk binary.
 */
static void meta_write(const DataItem *it)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/objects/%.2s/%s.meta",
             g_data_dir, it->hash, it->hash);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "name=%s\nauthor=%s\nn_samples=%d\n"
        "status=%d\nsubmitted_at=%ld\napproved_at=%ld\n"
        "last_stale_alert=%ld\nserved_count=%d\n",
        it->name, it->author, it->n_samples,
        it->status, (long)it->submitted_at, (long)it->approved_at,
        (long)it->last_stale_alert, it->served_count);
    fclose(f);
}

/*
 * Read a .meta file for the given hash into *out.  The hash field is
 * pre-populated by the caller; all other fields are filled from the file.
 * Returns 0 on success, -1 if the file does not exist or cannot be parsed.
 */
static int meta_read(const char *hash, DataItem *out)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/objects/%.2s/%s.meta",
             g_data_dir, hash, hash);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(out, 0, sizeof(*out));
    snprintf(out->hash, sizeof(out->hash), "%s", hash);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "name") == 0)
            snprintf(out->name, sizeof(out->name), "%s", val);
        else if (strcmp(key, "author") == 0)
            snprintf(out->author, sizeof(out->author), "%s", val);
        else if (strcmp(key, "n_samples") == 0)
            out->n_samples = atoi(val);
        else if (strcmp(key, "status") == 0)
            out->status = atoi(val);
        else if (strcmp(key, "submitted_at") == 0)
            out->submitted_at = (time_t)atol(val);
        else if (strcmp(key, "approved_at") == 0)
            out->approved_at = (time_t)atol(val);
        else if (strcmp(key, "last_stale_alert") == 0)
            out->last_stale_alert = (time_t)atol(val);
        else if (strcmp(key, "served_count") == 0)
            out->served_count = atoi(val);
    }
    fclose(f);
    return 0;
}

/*
 * Add a DataItem that was read from disk on startup.  Unlike item_add(),
 * this preserves the existing status, timestamps, and served_count.
 * Returns a pointer to the placed item, or NULL if the registry is full.
 */
static DataItem *item_add_restored(const DataItem *src)
{
    if (g_item_count >= MAX_DATA_ITEMS) return NULL;
    DataItem *it = &g_items[g_item_count++];
    *it = *src;
    return it;
}

/*
 * Walk the objects directory and reconstruct g_items[] from .meta sidecar
 * files.  Only objects whose .obj file still exists on disk are restored.
 * No-op on Windows (registry starts empty; items are re-ingested over MQTT).
 */
static void scan_and_restore(void)
{
#ifndef COMPAT_WINDOWS
    char obj_dir[300];
    snprintf(obj_dir, sizeof(obj_dir), "%s/objects", g_data_dir);

    DIR *top = opendir(obj_dir);
    if (!top) return;

    struct dirent *sub_ent;
    while ((sub_ent = readdir(top)) != NULL) {
        if (sub_ent->d_name[0] == '.') continue;

        char sub_path[512];
        snprintf(sub_path, sizeof(sub_path), "%s/%s",
                 obj_dir, sub_ent->d_name);

        DIR *sub = opendir(sub_path);
        if (!sub) continue;

        struct dirent *file_ent;
        while ((file_ent = readdir(sub)) != NULL) {
            const char *fname = file_ent->d_name;
            size_t flen = strlen(fname);
            if (flen < 6 || strcmp(fname + flen - 5, ".meta") != 0) continue;

            char hash[17] = {0};
            size_t hlen = flen - 5;
            if (hlen > 16) hlen = 16;
            memcpy(hash, fname, hlen);

            if (!storage_exists(g_data_dir, hash)) continue;

            DataItem item;
            if (meta_read(hash, &item) != 0) continue;

            DataItem *it = item_add_restored(&item);
            if (it)
                printf("[custodian] restored  hash=%s  name=%s  status=%d\n",
                       hash, item.name, item.status);
        }
        closedir(sub);
    }
    closedir(top);
    printf("[custodian] restored %d item(s) from disk\n", g_item_count);
#else
    printf("[custodian] registry restore not supported on Windows; starting empty\n");
#endif
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
    DataItem *it = NULL;
    if (g_item_count >= MAX_DATA_ITEMS) {
        /* Pool full: evict the first rejected slot to make room */
        for (int i = 0; i < g_item_count; i++) {
            if (g_items[i].status == ITEM_REJECTED) {
                it = &g_items[i];
                break;
            }
        }
        if (!it) return NULL;  /* no rejected slots to evict */
    } else {
        it = &g_items[g_item_count++];
    }
    memset(it, 0, sizeof(*it));
    snprintf(it->hash, sizeof(it->hash), "%s", hash);
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->author, sizeof(it->author), "%s", author);
    it->n_samples    = n_samples;
    it->status       = ITEM_PENDING;
    it->submitted_at = time(NULL);
    meta_write(it);
    return it;
}

/* ── Manifest helpers ────────────────────────────────────────────────── */

/*
 * Check if a stored object's content begins with the manifest type marker.
 * Returns 1 if it is a manifest, 0 otherwise.
 */
static int is_manifest_hash(const char *hash)
{
    char probe[32];
    int n = storage_get(g_data_dir, hash, probe, sizeof(probe));
    if (n < 20) return 0;
    return strncmp(probe, "{\"type\":\"manifest\"", 18) == 0;
}

/*
 * Iterate over the "shards":[...] array in a manifest JSON string and call
 * cb(shard_hash, userdata) for each 16-char hex hash.
 */
typedef void (*shard_cb)(const char *shard_hash, void *ud);

static void manifest_each_shard(const char *manifest_json,
                                shard_cb cb, void *ud)
{
    const char *p = strstr(manifest_json, "\"shards\":[");
    if (!p) return;
    p += 10; /* skip past "[" */
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p == '"') {
            p++;
            char h[17] = {0};
            int i = 0;
            while (*p && *p != '"' && i < 16) h[i++] = *p++;
            if (*p == '"') p++;
            if (i == 16) cb(h, ud);
        } else {
            p++;
        }
    }
}

static void approve_shard_cb(const char *shard_hash, void *ud)
{
    (void)ud;
    DataItem *s = item_find(shard_hash);
    if (!s) return;
    if (s->status == ITEM_SHARD || s->status == ITEM_PENDING) {
        s->status      = ITEM_APPROVED;
        s->approved_at = time(NULL);
        meta_write(s);
    }
}

static void reject_shard_cb(const char *shard_hash, void *ud)
{
    (void)ud;
    DataItem *s = item_find(shard_hash);
    if (s) { s->status = ITEM_REJECTED; meta_write(s); }
}

/* ── Approval + ready notification ──────────────────────────────────── */

static void on_approved(const char *hash)
{
    DataItem *it = item_find(hash);
    if (!it) return;
    if (it->status == ITEM_APPROVED) return;

    it->status      = ITEM_APPROVED;
    it->approved_at = time(NULL);

    /* If this is a manifest, promote all its shards too */
    if (is_manifest_hash(hash)) {
        char mj[MANIFEST_MAX_SHARDS * 20 + 512];
        int n = storage_get(g_data_dir, hash, mj, sizeof(mj));
        if (n > 0) manifest_each_shard(mj, approve_shard_cb, NULL);
    }

    /* Record approval in the transaction chain */
    char chain_payload[64];
    int cp_len = snprintf(chain_payload, sizeof(chain_payload),
                          "{\"hash\":\"%s\"}", hash);
    chain_append(&g_chain, CHAIN_TX_DATA_APPROVE, chain_payload, (size_t)cp_len);

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
    meta_write(it);
    printf("[custodian] data ready  hash=%s  name=%s  samples=%d\n",
           it->hash, it->name, it->n_samples);
}

static void on_rejected(const char *hash)
{
    DataItem *it = item_find(hash);
    if (!it) return;
    it->status = ITEM_REJECTED;
    meta_write(it);

    /* If this is a manifest, also reject all its shards */
    if (is_manifest_hash(hash)) {
        char mj[MANIFEST_MAX_SHARDS * 20 + 512];
        int n = storage_get(g_data_dir, hash, mj, sizeof(mj));
        if (n > 0) manifest_each_shard(mj, reject_shard_cb, NULL);
    }

    /* Record rejection in the transaction chain */
    char chain_payload[64];
    int cp_len = snprintf(chain_payload, sizeof(chain_payload),
                          "{\"hash\":\"%s\"}", hash);
    chain_append(&g_chain, CHAIN_TX_DATA_REJECT, chain_payload, (size_t)cp_len);
}

/* ── Staleness monitoring ────────────────────────────────────────────── */

static void check_staleness(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_item_count; i++) {
        DataItem *it = &g_items[i];
        if (it->status != ITEM_APPROVED) continue;
        time_t age = now - it->approved_at;
        if (age < g_stale_after) continue;
        /* Cooldown: don't re-alert until another full stale_after interval */
        if (it->last_stale_alert > 0 &&
            (now - it->last_stale_alert) < g_stale_after) continue;

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

        it->last_stale_alert = now;
    }
}

/* ── Ledger handlers live in edge/ledger_http.c (shared module) ───────── */

#include "../../edge/ledger_http.h"

/* ── HTTP server ─────────────────────────────────────────────────────── */

static compat_socket_t g_http_fd = COMPAT_INVALID_SOCKET;

/* http_listen / http_send / http_send_html / http_send_binary /
 * http_recv_full all live in edge/http_util.c — shared across roles. */

static void handle_http(compat_socket_t cfd)
{
    /* Bound recv/send time so a slow client can't stall the MQTT event loop */
    struct timeval client_tv = {10, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO,
               COMPAT_SOCKOPT_CAST(&client_tv), sizeof(client_tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO,
               COMPAT_SOCKOPT_CAST(&client_tv), sizeof(client_tv));

    /* Static: avoids 256KB on the stack; safe because server is single-threaded */
    static char req[HTTP_BUF_SZ];
    int n = http_recv_full(cfd, req, sizeof(req), HTTP_MAX_BODY_SZ);
    if (n == -2) {
        http_send(cfd, 413, "{\"error\":\"payload too large\"}");
        compat_close_socket(cfd); return;
    }
    if (n <= 0) { compat_close_socket(cfd); return; }

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
        int approved = 0;
        for (int i = 0; i < g_item_count; i++)
            if (g_items[i].status == ITEM_APPROVED) approved++;
        char body[256];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"items\":%d,\"approved\":%d}",
            g_agent_name, g_item_count, approved);
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /pool — list all data items (shards are internal, excluded) */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/pool") == 0) {
        char body[HTTP_BUF_SZ / 2];
        int pos = 0, n;
        n = snprintf(body, sizeof(body), "[");
        if (n > 0 && (size_t)n < sizeof(body)) pos = n;
        int first = 1;
        for (int i = 0; i < g_item_count; i++) {
            const DataItem *it = &g_items[i];
            if (it->status == ITEM_SHARD) continue;
            if (!first) {
                n = snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
                if (n > 0 && (size_t)(pos + n) < sizeof(body)) pos += n;
            }
            first = 0;

            /* Detect manifests and include shard_count */
            char extra[64] = "";
            char probe[256];
            int pn = storage_get(g_data_dir, it->hash, probe, sizeof(probe) - 1);
            if (pn > 18 && strncmp(probe, "{\"type\":\"manifest\"", 18) == 0) {
                probe[pn] = '\0';
                int sc = 0;
                const char *scp = strstr(probe, "\"shard_count\":");
                if (scp) sc = atoi(scp + 14);
                snprintf(extra, sizeof(extra),
                         ",\"manifest\":true,\"shard_count\":%d", sc);
            }

            n = snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"hash\":\"%s\",\"name\":\"%s\","
                 "\"author\":\"%s\",\"n_samples\":%d,"
                 "\"status\":\"%s\",\"served\":%d%s}",
                it->hash, it->name, it->author, it->n_samples,
                it->status == ITEM_APPROVED ? "approved"
                : it->status == ITEM_REJECTED ? "rejected" : "pending",
                it->served_count, extra);
            if (n > 0 && (size_t)(pos + n) < sizeof(body)) pos += n;
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /objects/<hash>[?offset=N&count=M] — serve stored NebulaDisk object to Workers
     * Without query params: returns the full .obj file (NebulaDisk binary).
     * With offset/count: returns a slice of the raw float32 content payload. */
    if (strcmp(method, "GET") == 0 &&
        strncmp(path, "/objects/", 9) == 0) {
        const char *hash = path + 9;
        /* Split hash from query string */
        char clean_hash[17] = {0};
        const char *qstr = NULL;
        int j = 0;
        while (hash[j] && j < 16) {
            if (hash[j] == '?') { qstr = hash + j + 1; break; }
            clean_hash[j] = hash[j]; j++;
        }
        clean_hash[j] = '\0';

        if (!storage_exists(g_data_dir, clean_hash)) {
            http_send(cfd, 404, "{\"error\":\"not found\"}");
            compat_close_socket(cfd); return;
        }

        {
            DataItem *auth_it = item_find(clean_hash);
            if (auth_it && auth_it->status != ITEM_APPROVED) {
                http_send(cfd, 403, "{\"error\":\"not approved\"}");
                compat_close_socket(cfd); return;
            }
        }

        char obj_path_buf[512];
        if (storage_path(g_data_dir, clean_hash,
                         obj_path_buf, sizeof(obj_path_buf)) < 0) {
            http_send(cfd, 500, "{\"error\":\"storage error\"}");
            compat_close_socket(cfd); return;
        }

        /* Parse optional offset= and count= query parameters */
        int range_offset = -1, range_count = -1;
        if (qstr) {
            const char *op = strstr(qstr, "offset=");
            const char *cp = strstr(qstr, "count=");
            if (op) range_offset = atoi(op + 7);
            if (cp) range_count  = atoi(cp + 6);
        }

        if (range_offset >= 0 && range_count > 0) {
            /* Range serving: return raw float32 slice from content payload */
            long content_off = storage_content_offset(g_data_dir, clean_hash);
            int  content_len = storage_content_len(g_data_dir, clean_hash);
            if (content_off < 0 || content_len < 0) {
                http_send(cfd, 500, "{\"error\":\"storage error\"}");
                compat_close_socket(cfd); return;
            }
            int total_floats = content_len / (int)sizeof(float);
            if (range_offset >= total_floats) {
                http_send(cfd, 416, "{\"error\":\"range not satisfiable\"}");
                compat_close_socket(cfd); return;
            }
            /* Clamp count to available floats */
            if (range_offset + range_count > total_floats)
                range_count = total_floats - range_offset;

            FILE *f = fopen(obj_path_buf, "rb");
            if (!f) {
                http_send(cfd, 500, "{\"error\":\"read error\"}");
                compat_close_socket(cfd); return;
            }
            long byte_start = content_off + (long)range_offset * (long)sizeof(float);
            fseek(f, byte_start, SEEK_SET);
            size_t slice_bytes = (size_t)range_count * sizeof(float);
            char *slice_buf = malloc(slice_bytes);
            if (!slice_buf) {
                fclose(f);
                http_send(cfd, 500, "{\"error\":\"oom\"}");
                compat_close_socket(cfd); return;
            }
            size_t rd = fread(slice_buf, 1, slice_bytes, f);
            fclose(f);

            DataItem *it = item_find(clean_hash);
            if (it) { it->served_count++; meta_write(it); }

            http_send_binary(cfd, "application/octet-stream", slice_buf, rd);
            free(slice_buf);
            compat_close_socket(cfd); return;
        }

        /* Full object: stream the raw .obj file */
        FILE *f = fopen(obj_path_buf, "rb");
        if (!f) {
            http_send(cfd, 500, "{\"error\":\"read error\"}");
            compat_close_socket(cfd); return;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *file_buf = malloc((size_t)fsz);
        if (!file_buf) {
            fclose(f);
            http_send(cfd, 500, "{\"error\":\"oom\"}");
            compat_close_socket(cfd); return;
        }
        size_t rd = fread(file_buf, 1, (size_t)fsz, f);
        fclose(f);

        /* Update served count and persist */
        DataItem *it = item_find(clean_hash);
        if (it) { it->served_count++; meta_write(it); }

        http_send_binary(cfd, "application/octet-stream", file_buf, rd);
        free(file_buf);
        compat_close_socket(cfd); return;
    }

    /*
     * POST /ingest  — accept raw data and transform into NebulaDisk tensor.
     *
     * Body (JSON):
     *   {"name": "dataset-label", "data": [1.0, 2.0, ...],
     *    "format": "json|csv", "features": N}
     *
     * If n_floats > SHARD_FLOAT_COUNT the payload is automatically split into
     * shards (raw float32 binary blobs) and a manifest object is created.
     * The manifest is submitted to Sentient for approval; shards bypass quarantine.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/ingest") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;

        char name[64]   = "unnamed";
        char format[16] = "json";
        char feat_str[16] = "1";
        json_str(body_start, "name",     name,     sizeof(name));
        json_str(body_start, "format",   format,   sizeof(format));
        json_str(body_start, "features", feat_str, sizeof(feat_str));
        int features = atoi(feat_str);
        if (features < 1) features = 1;

        /* Find the "data" field value */
        const char *data_start = strstr(body_start, "\"data\":");
        if (!data_start) {
            http_send(cfd, 400, "{\"error\":\"data field required\"}");
            compat_close_socket(cfd); return;
        }
        data_start += 7;
        while (*data_start == ' ') data_start++;

        /* Parse all floats — no per-object cap here */
        int max_floats = MANIFEST_MAX_SHARDS * SHARD_FLOAT_COUNT;
        float *floats = (float *)malloc((size_t)max_floats * sizeof(float));
        if (!floats) {
            http_send(cfd, 500, "{\"error\":\"oom\"}");
            compat_close_socket(cfd); return;
        }
        int n_floats = 0;
        if (strcmp(format, "csv") == 0) {
            if (*data_start == '"') data_start++;
            n_floats = parse_csv(data_start, floats, max_floats);
        } else {
            n_floats = parse_floats(data_start, floats, max_floats);
        }

        if (n_floats <= 0) {
            free(floats);
            http_send(cfd, 400, "{\"error\":\"no numeric data found\"}");
            compat_close_socket(cfd); return;
        }

        char resp_hash[17];
        char resp[512];

        if (n_floats <= SHARD_FLOAT_COUNT) {
            /* ── Single object path ── */
            char hash[17];
            uint32_t shape[1] = { (uint32_t)n_floats };
            if (storage_put_shaped(g_data_dir, (const char *)floats,
                            (size_t)n_floats * sizeof(float),
                            STORAGE_OBJ_DATA, STORAGE_DTYPE_FLOAT32,
                            1, shape,
                            g_agent_name, name, hash) < 0) {
                free(floats);
                http_send(cfd, 500, "{\"error\":\"storage failed\"}");
                compat_close_socket(cfd); return;
            }
            DataItem *it = item_find(hash);
            if (!it) {
                it = item_add(hash, name, g_agent_name, n_floats);
                if (!it) {
                    free(floats);
                    http_send(cfd, 503, "{\"error\":\"pool full\"}");
                    compat_close_socket(cfd); return;
                }
            }

            /* Record ingestion in the transaction chain */
            char chain_payload[256];
            int cp_len = snprintf(chain_payload, sizeof(chain_payload),
                "{\"hash\":\"%s\",\"name\":\"%s\",\"n_samples\":%d}",
                hash, name, n_floats);
            chain_append(&g_chain, CHAIN_TX_DATA_SUBMIT,
                         chain_payload, (size_t)cp_len);

            char sentient_body[512];
            snprintf(sentient_body, sizeof(sentient_body),
                "{\"name\":\"%s\",\"hash\":\"%s\","
                 "\"n_samples\":%d,\"author\":\"%s\"}",
                name, hash, n_floats, g_agent_name);
            char sentient_resp[512];
            http_post(g_sentient_host, g_sentient_port,
                      "/data/submit", sentient_body,
                      sentient_resp, sizeof(sentient_resp));
            snprintf(resp, sizeof(resp),
                "{\"hash\":\"%s\",\"n_samples\":%d,\"status\":\"pending\"}",
                hash, n_floats);
            printf("[custodian] ingested '%s'  samples=%d  hash=%s\n",
                   name, n_floats, hash);
            snprintf(resp_hash, sizeof(resp_hash), "%s", hash);
        } else {
            /* ── Sharded manifest path ── */
            int n_shards = (n_floats + SHARD_FLOAT_COUNT - 1) / SHARD_FLOAT_COUNT;
            if (n_shards > MANIFEST_MAX_SHARDS) {
                free(floats);
                http_send(cfd, 400, "{\"error\":\"dataset exceeds manifest capacity\"}");
                compat_close_socket(cfd); return;
            }

            /* Collect shard hashes */
            char (*shard_hashes)[17] = (char (*)[17])malloc(
                (size_t)n_shards * 17);
            if (!shard_hashes) {
                free(floats);
                http_send(cfd, 500, "{\"error\":\"oom\"}");
                compat_close_socket(cfd); return;
            }

            for (int s = 0; s < n_shards; s++) {
                int shard_start = s * SHARD_FLOAT_COUNT;
                int shard_len   = (shard_start + SHARD_FLOAT_COUNT <= n_floats)
                                  ? SHARD_FLOAT_COUNT
                                  : n_floats - shard_start;
                char shard_name[80];
                snprintf(shard_name, sizeof(shard_name), "%s_shard_%d", name, s);

                char shash[17];
                uint32_t shard_shape[1] = { (uint32_t)shard_len };
                if (storage_put_shaped(g_data_dir,
                                (const char *)(floats + shard_start),
                                (size_t)shard_len * sizeof(float),
                                STORAGE_OBJ_DATA, STORAGE_DTYPE_FLOAT32,
                                1, shard_shape,
                                g_agent_name, shard_name,
                                shash) < 0) {
                    free(shard_hashes); free(floats);
                    http_send(cfd, 500, "{\"error\":\"shard storage failed\"}");
                    compat_close_socket(cfd); return;
                }
                memcpy(shard_hashes[s], shash, 17);

                /* Track shard as ITEM_SHARD — bypasses quarantine */
                DataItem *sit = item_find(shash);
                if (!sit) {
                    sit = item_add(shash, shard_name, g_agent_name, shard_len);
                    if (!sit) {
                        free(shard_hashes); free(floats);
                        http_send(cfd, 503, "{\"error\":\"pool full\"}");
                        compat_close_socket(cfd); return;
                    }
                    sit->status = ITEM_SHARD;
                    meta_write(sit);
                }
            }

            /* Build manifest JSON */
            /* Estimate size: header ~256 + n_shards * 18 chars per hash */
            size_t mj_sz = 512 + (size_t)n_shards * 20;
            char *mj = (char *)malloc(mj_sz);
            if (!mj) {
                free(shard_hashes); free(floats);
                http_send(cfd, 500, "{\"error\":\"oom\"}");
                compat_close_socket(cfd); return;
            }
            int mpos = snprintf(mj, mj_sz,
                "{\"type\":\"manifest\",\"name\":\"%s\","
                 "\"total_floats\":%d,\"features\":%d,"
                 "\"shard_float_count\":%d,\"shard_count\":%d,"
                 "\"custodian_host\":\"%s\",\"custodian_port\":%u,"
                 "\"shards\":[",
                name, n_floats, features,
                SHARD_FLOAT_COUNT, n_shards,
                g_sentient_host[0] ? "127.0.0.1" : "127.0.0.1",
                g_http_port);
            for (int s = 0; s < n_shards; s++) {
                if (mpos + 20 >= (int)mj_sz) {
                    /* Grow manifest buffer */
                    mj_sz *= 2;
                    char *tmp = (char *)realloc(mj, mj_sz);
                    if (!tmp) { free(mj); free(shard_hashes); free(floats);
                                http_send(cfd, 500, "{\"error\":\"oom\"}");
                                compat_close_socket(cfd); return; }
                    mj = tmp;
                }
                int sn = snprintf(mj + mpos, mj_sz - (size_t)mpos,
                    "%s\"%s\"", s > 0 ? "," : "", shard_hashes[s]);
                if (sn > 0 && (size_t)(mpos + sn) < mj_sz) mpos += sn;
            }
            { int sn = snprintf(mj + mpos, mj_sz - (size_t)mpos, "]}");
              if (sn > 0 && (size_t)(mpos + sn) < mj_sz) mpos += sn; }

            char mhash[17];
            if (storage_put(g_data_dir, mj, (size_t)mpos,
                            STORAGE_OBJ_DATA, g_agent_name, name, mhash) < 0) {
                free(mj); free(shard_hashes); free(floats);
                http_send(cfd, 500, "{\"error\":\"manifest storage failed\"}");
                compat_close_socket(cfd); return;
            }
            free(mj);
            free(shard_hashes);

            DataItem *mit = item_find(mhash);
            if (!mit) {
                mit = item_add(mhash, name, g_agent_name, n_floats);
                if (!mit) {
                    free(floats);
                    http_send(cfd, 503, "{\"error\":\"pool full\"}");
                    compat_close_socket(cfd); return;
                }
            }

            /* Record ingestion in the transaction chain (manifest hash only;
             * shards are an internal storage artifact, not a submission). */
            {
                char chain_payload[256];
                int cp_len = snprintf(chain_payload, sizeof(chain_payload),
                    "{\"hash\":\"%s\",\"name\":\"%s\","
                     "\"n_samples\":%d,\"shards\":%d}",
                    mhash, name, n_floats, n_shards);
                chain_append(&g_chain, CHAIN_TX_DATA_SUBMIT,
                             chain_payload, (size_t)cp_len);
            }

            char sentient_body[512];
            snprintf(sentient_body, sizeof(sentient_body),
                "{\"name\":\"%s\",\"hash\":\"%s\","
                 "\"n_samples\":%d,\"author\":\"%s\"}",
                name, mhash, n_floats, g_agent_name);
            char sentient_resp[512];
            http_post(g_sentient_host, g_sentient_port,
                      "/data/submit", sentient_body,
                      sentient_resp, sizeof(sentient_resp));

            snprintf(resp, sizeof(resp),
                "{\"hash\":\"%s\",\"n_samples\":%d,"
                 "\"status\":\"pending\",\"manifest\":true,\"shards\":%d}",
                mhash, n_floats, n_shards);
            printf("[custodian] ingested manifest '%s'  samples=%d"
                   "  shards=%d  hash=%s\n",
                   name, n_floats, n_shards, mhash);
            snprintf(resp_hash, sizeof(resp_hash), "%s", mhash);
        }

        free(floats);
        http_send(cfd, 201, resp);
        compat_close_socket(cfd); return;
    }

    /*
     * POST /approve  body: {"hash": "<16hex>"}
     * Manually approve a pending item (for testing or admin use).
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/approve") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) { http_send(cfd, 400, "{\"error\":\"no body\"}"); compat_close_socket(cfd); return; }
        body_start += 4;
        char hash[17] = {0};
        json_str(body_start, "hash", hash, sizeof(hash));
        if (hash[0] == '\0') {
            http_send(cfd, 400, "{\"error\":\"hash required\"}"); compat_close_socket(cfd); return;
        }
        on_approved(hash);
        http_send(cfd, 200, "{\"ok\":true}");
        compat_close_socket(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ / 4];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* DELETE /objects/<hash> — remove object from disk and registry */
    if (strcmp(method, "DELETE") == 0 &&
        strncmp(path, "/objects/", 9) == 0) {
        const char *hash = path + 9;
        char clean_hash[17] = {0};
        int j = 0;
        while (hash[j] && hash[j] != '?' && j < 16) {
            clean_hash[j] = hash[j]; j++;
        }
        clean_hash[j] = '\0';

        if (!storage_exists(g_data_dir, clean_hash)) {
            http_send(cfd, 404, "{\"error\":\"not found\"}");
            compat_close_socket(cfd); return;
        }

        /* If this is a manifest, cascade-delete all its shards first */
        {
            char mj[MANIFEST_MAX_SHARDS * 20 + 512];
            int mj_len = storage_get(g_data_dir, clean_hash, mj, sizeof(mj) - 1);
            if (mj_len > 18 && strncmp(mj, "{\"type\":\"manifest\"", 18) == 0) {
                mj[mj_len] = '\0';
                /* Collect shard hashes before modifying g_items[] */
                char shard_list[MANIFEST_MAX_SHARDS][17];
                int  shard_list_n = 0;
                const char *p = strstr(mj, "\"shards\":[");
                if (p) {
                    p += 10;
                    while (*p && shard_list_n < MANIFEST_MAX_SHARDS) {
                        while (*p == ' ' || *p == ',') p++;
                        if (*p == ']' || *p == '\0') break;
                        if (*p == '"') {
                            p++;
                            int ci = 0;
                            while (*p && *p != '"' && ci < 16)
                                shard_list[shard_list_n][ci++] = *p++;
                            shard_list[shard_list_n][ci] = '\0';
                            if (*p == '"') p++;
                            if (ci == 16) shard_list_n++;
                        } else { p++; }
                    }
                }
                for (int s = 0; s < shard_list_n; s++) {
                    char sobj[512], smeta[512];
                    if (storage_path(g_data_dir, shard_list[s], sobj, sizeof(sobj)) == 0)
                        remove(sobj);
                    snprintf(smeta, sizeof(smeta), "%.255s/objects/%.2s/%.16s.meta",
                             g_data_dir, shard_list[s], shard_list[s]);
                    remove(smeta);
                    for (int k = 0; k < g_item_count; k++) {
                        if (strcmp(g_items[k].hash, shard_list[s]) == 0) {
                            int tail = g_item_count - k - 1;
                            if (tail > 0)
                                memmove(&g_items[k], &g_items[k + 1],
                                        (size_t)tail * sizeof(DataItem));
                            g_item_count--;
                            break;
                        }
                    }
                }
                printf("[custodian] deleted %d shard(s) for manifest %s\n",
                       shard_list_n, clean_hash);
            }
        }

        /* Remove manifest/object .obj and .meta files from disk */
        char obj_path[512];
        if (storage_path(g_data_dir, clean_hash,
                         obj_path, sizeof(obj_path)) == 0) {
            remove(obj_path);
        }
        char meta_fpath[512];
        snprintf(meta_fpath, sizeof(meta_fpath), "%s/objects/%.2s/%s.meta",
                 g_data_dir, clean_hash, clean_hash);
        remove(meta_fpath);

        /* Remove from in-memory registry by compacting the array */
        for (int i = 0; i < g_item_count; i++) {
            if (strcmp(g_items[i].hash, clean_hash) == 0) {
                int tail = g_item_count - i - 1;
                if (tail > 0)
                    memmove(&g_items[i], &g_items[i + 1],
                            (size_t)tail * sizeof(DataItem));
                g_item_count--;
                break;
            }
        }

        printf("[custodian] deleted  hash=%s\n", clean_hash);
        http_send(cfd, 200, "{\"ok\":true}");
        compat_close_socket(cfd); return;
    }

    /* GET /ledger/verify — shared handler */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/ledger/verify") == 0) {
        ledger_http_serve_verify(cfd, &g_chain);
        compat_close_socket(cfd); return;
    }

    /* GET /ledger[?offset=N&limit=M] — shared handler */
    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/ledger") == 0 ||
         strncmp(path, "/ledger?", 8) == 0)) {
        const char *qs = strchr(path, '?');
        ledger_http_serve_index(cfd, &g_chain, qs ? qs + 1 : NULL);
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
            snprintf(g_broker_host, sizeof(g_broker_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc)
            g_broker_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_http_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc)
            snprintf(g_data_dir, sizeof(g_data_dir), "%s", argv[++i]);
        else if (strcmp(argv[i], "--sentient") == 0 && i + 1 < argc)
            snprintf(g_sentient_host, sizeof(g_sentient_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--sentient-port") == 0 && i + 1 < argc)
            g_sentient_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--stale-after") == 0 && i + 1 < argc)
            g_stale_after = (time_t)atoi(argv[++i]);
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
    printf("[custodian] identity  machine=%s  node=%s\n",
           g_machine_hash_hex, g_node_id_hex);

    /* ── Storage ── */
    ensure_data_dir();

    /* ── Transaction chain ── */
    if (chain_open(g_data_dir, g_agent_name, &g_chain) != 0) {
        fprintf(stderr, "[custodian] chain_open failed — refusing to start. "
                        "Inspect or remove %s/agents/%s/chain.binlog.\n",
                g_data_dir, g_agent_name);
        return 1;
    }
    printf("[custodian] chain  path=%s  next_tx_id=%llu\n",
           g_chain.path, (unsigned long long)g_chain.next_tx_id);

    /* ── Init tables ── */
    peer_table_init(&g_peers);
    memset(g_items, 0, sizeof(g_items));
    scan_and_restore();

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
    if (g_http_fd == COMPAT_INVALID_SOCKET) {
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
        if (select(COMPAT_SELECT_NFDS(g_http_fd), &rfds, NULL, NULL, &tv) > 0 &&
            FD_ISSET(g_http_fd, &rfds)) {
            compat_socket_t cfd = accept(g_http_fd, NULL, NULL);
            if (cfd != COMPAT_INVALID_SOCKET) handle_http(cfd);
        }

        /* Sync MQTT I/O */
        mqtt_transport_sync(&g_mqtt, 0);

        /* Dispatch incoming MQTT messages */
        uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
        char    sender_ip[46];
        char    topic[128];
        int     pkt_len;
        while ((pkt_len = mqtt_transport_recv_ex(&g_mqtt, pkt,
                                                 sizeof(pkt), sender_ip,
                                                 topic)) > 0) {
            /* Plain JSON approval/rejection from Sentient */
            if (strcmp(topic, "nml/data/approve") == 0) {
                char hash[17] = {0};
                json_str((const char *)pkt, "hash", hash, sizeof(hash));
                if (hash[0]) on_approved(hash);
                continue;
            }
            if (strcmp(topic, "nml/data/reject") == 0) {
                char hash[17] = {0};
                json_str((const char *)pkt, "hash", hash, sizeof(hash));
                if (hash[0]) on_rejected(hash);
                continue;
            }

            /* NML wire-format messages (heartbeats, etc.) */
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
    chain_close(&g_chain);
    if (g_http_fd != COMPAT_INVALID_SOCKET) compat_close_socket(g_http_fd);
    compat_winsock_cleanup();
    return 0;
}
