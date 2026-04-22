/*
 * worker_agent.c — NML Collective Worker role.
 *
 * Receives signed NML programs, executes them against local data, and
 * publishes results back to the mesh.
 *
 * Transport (selected at startup):
 *   MQTT — primary; connect to Herald broker via --broker HOST
 *   UDP  — fallback; multicast on LAN when --broker is omitted
 *
 * Data cache:
 *   Up to DATA_CACHE_MAX named datasets held in memory.  Datasets larger
 *   than DATA_MMAP_THRESHOLD bytes are backed by anonymous mmap so the OS
 *   can page them out under memory pressure.  Entries are evicted LRU.
 *
 *   nml/data/ready  → pre-fetch (or invalidate if hash changed)
 *   nml/data/stale  → evict; re-fetched lazily on next program
 *   nml/data/reject → evict and mark; not re-fetched
 *
 * Usage:
 *   ./worker_agent [--name NAME] [--port PORT] [--data FILE]
 *                  [--data-name NAME] [--require-signed]
 *                  [--broker HOST] [--broker-port PORT]
 */

#define _POSIX_C_SOURCE 200809L
/* Expose BSD/Darwin extensions (MAP_ANON) on Apple platforms */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/udp.h"
#include "../../edge/crypto.h"
#include "../../edge/nml_exec.h"
#include "../../edge/report.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/vote.h"
#include "../../edge/http_client.h"
#include "../../edge/http_util.h"
#include "../../edge/mqtt_transport.h"

/* Embedded landing page, generated from ui.html via `xxd -i`. */
#include "ui.html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat.h"

#ifdef COMPAT_POSIX
#include <sys/mman.h>
#endif

/* ── Data cache constants ────────────────────────────────────────────── */

#define DATA_CACHE_MAX       8
#define DATA_MMAP_THRESHOLD  (256 * 1024)   /* mmap datasets larger than 256 KB */
#define SHARD_BUF_SZ         (64 * 1024)    /* buffer for a single shard fetch */
#define STREAM_THRESHOLD     (1000000)      /* float count above which streaming is used */

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static PeerTable     g_peers;
static VoteTable     g_votes;

/* Transport — one or the other is active */
static int           g_use_mqtt   = 0;
static MQTTTransport g_mqtt;
static UDPContext    g_udp;

/* Agent identity */
static const char   *g_agent_name = EDGE_AGENT_NAME;
static uint16_t      g_agent_port = EDGE_HTTP_PORT;

/* Default dataset name (from --data-name or "__local__" for --data FILE) */
static const char   *g_data_name  = NULL;

/* ── Data cache ──────────────────────────────────────────────────────── */

typedef struct {
    char    name[64];     /* dataset name key                               */
    char    hash[17];     /* content hash for invalidation                  */
    char   *ptr;          /* data pointer (heap or mmap)                    */
    size_t  size;         /* byte length of data (excluding NUL terminator) */
    int     is_mmap;      /* 1 = munmap on evict, 0 = free                  */
    int     valid;        /* 1 = ready to use                               */
    int     rejected;     /* 1 = evicted by nml/data/reject, skip re-fetch  */
    time_t  last_used;
} DataCacheEntry;

static DataCacheEntry g_cache[DATA_CACHE_MAX];

/* ── Result history (for GET /results) ───────────────────────────────── */

#define RESULT_HISTORY_MAX 32

typedef struct {
    char   phash[17];
    char   score[32];
    time_t ts;
} ResultEntry;

static ResultEntry g_results[RESULT_HISTORY_MAX];
static int         g_results_head  = 0;   /* next slot to write */
static int         g_results_count = 0;   /* total valid entries (<= MAX) */

static void result_record(const char *phash, const char *score)
{
    ResultEntry *e = &g_results[g_results_head];
    snprintf(e->phash, sizeof(e->phash), "%s", phash ? phash : "");
    snprintf(e->score, sizeof(e->score), "%s", score ? score : "");
    e->ts = time(NULL);
    g_results_head = (g_results_head + 1) % RESULT_HISTORY_MAX;
    if (g_results_count < RESULT_HISTORY_MAX) g_results_count++;
}

static compat_socket_t g_http_fd = COMPAT_INVALID_SOCKET;

static void cache_free_entry(DataCacheEntry *e)
{
    if (!e->ptr) return;
#ifdef COMPAT_POSIX
    if (e->is_mmap) munmap(e->ptr, e->size + 1);
    else
#endif
    free(e->ptr);
    e->ptr     = NULL;
    e->valid   = 0;
    e->is_mmap = 0;
    e->size    = 0;
}

static DataCacheEntry *cache_find_by_name(const char *name)
{
    for (int i = 0; i < DATA_CACHE_MAX; i++)
        if (g_cache[i].name[0] && strcmp(g_cache[i].name, name) == 0)
            return &g_cache[i];
    return NULL;
}

static DataCacheEntry *cache_find_by_hash(const char *hash)
{
    for (int i = 0; i < DATA_CACHE_MAX; i++)
        if (g_cache[i].name[0] && strcmp(g_cache[i].hash, hash) == 0)
            return &g_cache[i];
    return NULL;
}

/* Return the least-recently-used non-rejected slot, or slot 0 as fallback. */
static DataCacheEntry *cache_evict_lru(void)
{
    /* Prefer an empty slot first */
    for (int i = 0; i < DATA_CACHE_MAX; i++)
        if (!g_cache[i].name[0]) return &g_cache[i];

    DataCacheEntry *lru = &g_cache[0];
    for (int i = 1; i < DATA_CACHE_MAX; i++)
        if (g_cache[i].last_used < lru->last_used) lru = &g_cache[i];

    printf("[%s] cache evict LRU: '%s'\n", g_agent_name, lru->name);
    cache_free_entry(lru);
    lru->name[0]  = '\0';
    lru->hash[0]  = '\0';
    lru->rejected = 0;
    return lru;
}

/*
 * Store data into the cache under name/hash.  Takes ownership of the
 * malloc'd data pointer.  If size > DATA_MMAP_THRESHOLD (and on POSIX),
 * the data is moved into anonymous mmap and the heap buffer is freed.
 */
static DataCacheEntry *cache_store(const char *name, const char *hash,
                                   char *data, size_t size)
{
    DataCacheEntry *slot = cache_find_by_name(name);
    if (!slot) slot = cache_evict_lru();
    else cache_free_entry(slot);

    snprintf(slot->name, sizeof(slot->name), "%s", name);
    snprintf(slot->hash, sizeof(slot->hash), "%s", hash ? hash : "");
    slot->size      = size;
    slot->rejected  = 0;
    slot->last_used = time(NULL);

#ifdef COMPAT_POSIX
    if (size > DATA_MMAP_THRESHOLD) {
        void *m = mmap(NULL, size + 1, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m != MAP_FAILED) {
            memcpy(m, data, size);
            ((char *)m)[size] = '\0';
            free(data);
            slot->ptr     = (char *)m;
            slot->is_mmap = 1;
            slot->valid   = 1;
            printf("[%s] cached '%s' in mmap (%zu KB)\n",
                   g_agent_name, name, size / 1024);
            return slot;
        }
        /* mmap failed — fall through to heap */
    }
#endif
    slot->ptr     = data;
    slot->is_mmap = 0;
    slot->valid   = 1;
    printf("[%s] cached '%s' in heap (%zu bytes)\n", g_agent_name, name, size);
    return slot;
}

/* ── JSON helper ─────────────────────────────────────────────────────── */

/* json_str was previously defined locally; now shared via http_util. */
#define json_str http_json_str

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

/*
 * Parse the NebulaDisk binary header (magic NML\x02) and return a malloc'd
 * copy of the content section (NUL-terminated).  Returns NULL on error.
 */
static char *nebula_extract_content(const uint8_t *buf, size_t buf_len)
{
    if (buf_len < 4 || memcmp(buf, "NML\x02", 4) != 0)
        return NULL;

    size_t pos = 4 + 8;
    if (pos + 2 > buf_len) return NULL;
    pos++;
    uint8_t author_len = buf[pos++];
    pos += author_len + 8;
    if (pos + 1 > buf_len) return NULL;
    uint8_t ndims = buf[pos++];
    pos += (size_t)ndims * 4;
    if (pos + 1 + 4 > buf_len) return NULL;
    pos++;
    uint32_t content_len = ((uint32_t)buf[pos]   << 24) |
                           ((uint32_t)buf[pos+1] << 16) |
                           ((uint32_t)buf[pos+2] <<  8) |
                            (uint32_t)buf[pos+3];
    pos += 4;
    if (pos + content_len > buf_len) return NULL;

    char *out = malloc(content_len + 1);
    if (!out) return NULL;
    memcpy(out, buf + pos, content_len);
    out[content_len] = '\0';
    return out;
}

/*
 * Parse the byte length of the NebulaDisk content section without extracting it.
 * Returns the content length in bytes, or 0 on parse failure.
 */
static size_t nebula_content_len(const uint8_t *buf, size_t buf_len)
{
    if (buf_len < 4 || memcmp(buf, "NML\x02", 4) != 0) return 0;
    size_t pos = 4 + 8;
    if (pos + 2 > buf_len) return 0;
    pos++;                           /* obj_type */
    uint8_t author_len = buf[pos++];
    pos += author_len + 8;           /* author + timestamp */
    if (pos + 1 > buf_len) return 0;
    uint8_t ndims = buf[pos++];
    pos += (size_t)ndims * 4;
    if (pos + 5 > buf_len) return 0;
    pos++;                           /* dtype */
    uint32_t clen = ((uint32_t)buf[pos]   << 24) |
                    ((uint32_t)buf[pos+1] << 16) |
                    ((uint32_t)buf[pos+2] <<  8) |
                     (uint32_t)buf[pos+3];
    return (size_t)clen;
}

/*
 * Fetch all shards described in manifest_json from the originating custodian
 * and assemble them into a single NML data string
 * "@name shape=N,K dtype=f32 data=v0,v1,...".
 * Returns a malloc'd NUL-terminated string; caller owns it.
 * Returns NULL on any fatal error.
 */
static char *assemble_from_manifest(const char *manifest_json)
{
    char cust_host[128] = "127.0.0.1";
    char ds_name[64]    = "training_data";
    int  total_floats   = 0;
    int  features       = 1;
    uint16_t cust_port  = 9004;

    json_str(manifest_json, "custodian_host", cust_host, sizeof(cust_host));
    json_str(manifest_json, "name",           ds_name,   sizeof(ds_name));

    const char *p;
    p = strstr(manifest_json, "\"total_floats\":");
    if (p) total_floats = atoi(p + 15);
    p = strstr(manifest_json, "\"features\":");
    if (p) features = atoi(p + 11);
    p = strstr(manifest_json, "\"custodian_port\":");
    if (p) cust_port = (uint16_t)atoi(p + 17);

    if (total_floats <= 0) return NULL;
    if (features < 1) features = 1;

    /* Allocate output buffer — header + worst-case float ASCII */
    size_t out_sz = 128 + (size_t)total_floats * 14;
    char  *out    = (char *)malloc(out_sz);
    if (!out) return NULL;

    int pos = snprintf(out, out_sz, "@%s shape=%d,%d dtype=f32 data=",
                       ds_name, total_floats, features);

    char  *shard_buf = (char *)malloc(SHARD_BUF_SZ);
    if (!shard_buf) { free(out); return NULL; }

    /* Iterate shards[] array */
    const char *sp = strstr(manifest_json, "\"shards\":[");
    if (!sp) { free(shard_buf); free(out); return NULL; }
    sp += 10;

    int first_float = 1;
    while (*sp) {
        while (*sp == ' ' || *sp == ',') sp++;
        if (*sp == ']' || *sp == '\0') break;
        if (*sp != '"') { sp++; continue; }
        sp++;
        char shash[17] = {0};
        int hi = 0;
        while (*sp && *sp != '"' && hi < 16) shash[hi++] = *sp++;
        if (*sp == '"') sp++;
        if (hi != 16) continue;

        /* Fetch shard NebulaDisk object from custodian */
        char spath[64];
        snprintf(spath, sizeof(spath), "/objects/%s", shash);
        int sn = http_get(cust_host, cust_port, spath, shard_buf, SHARD_BUF_SZ);
        if (sn <= 0) {
            fprintf(stderr, "[%s] assemble: failed to fetch shard %s\n",
                    g_agent_name, shash);
            continue;
        }

        /* Determine float count from NebulaDisk content_len */
        size_t raw_bytes = nebula_content_len((const uint8_t *)shard_buf,
                                              (size_t)sn);
        int float_count = (int)(raw_bytes / sizeof(float));
        if (float_count <= 0) {
            fprintf(stderr, "[%s] assemble: bad shard header for %s\n",
                    g_agent_name, shash);
            continue;
        }

        /* Extract raw float32 bytes */
        char *raw = nebula_extract_content((const uint8_t *)shard_buf, (size_t)sn);
        if (!raw) {
            fprintf(stderr, "[%s] assemble: nebula extract failed for %s\n",
                    g_agent_name, shash);
            continue;
        }

        /* Grow output buffer if needed */
        size_t needed = (size_t)pos + (size_t)float_count * 15;
        if (needed >= out_sz) {
            out_sz = needed * 2;
            char *tmp = (char *)realloc(out, out_sz);
            if (!tmp) { free(raw); free(shard_buf); free(out); return NULL; }
            out = tmp;
        }

        /* Append floats as ASCII */
        const float *fptr = (const float *)raw;
        for (int fi = 0; fi < float_count; fi++) {
            if (!first_float) out[pos++] = ',';
            first_float = 0;
            int w = snprintf(out + pos, out_sz - (size_t)pos, "%.6g", fptr[fi]);
            if (w > 0 && (size_t)(pos + w) < out_sz) pos += w;
        }
        free(raw);
    }

    free(shard_buf);
    if ((size_t)pos < out_sz) out[pos] = '\0';
    printf("[%s] assembled manifest: %d floats (%zu bytes)\n",
           g_agent_name, total_floats, (size_t)pos);
    return out;
}

static const PeerEntry *find_sentient(void)
{
    for (int i = 0; i < g_peers.count; i++) {
        const PeerEntry *e = &g_peers.entries[i];
        if (e->active && !e->quarantined &&
            e->ip[0] != '\0' &&
            strncmp(e->name, "sentient", 8) == 0)
            return e;
    }
    return NULL;
}

/*
 * Fetch dataset by name from Sentient, store in cache, and return the entry.
 * If expected_hash is non-NULL and the cache already holds that hash, returns
 * the existing entry without a network round-trip.
 * Returns NULL on failure.
 */
static DataCacheEntry *fetch_and_cache(const char *name)
{
    const PeerEntry *sentient = find_sentient();
    if (!sentient) {
        fprintf(stderr, "[%s] no sentient peer to fetch '%s'\n",
                g_agent_name, name);
        return NULL;
    }

    size_t buf_sz = NML_MAX_TENSOR_SIZE + 8192;
    char  *buf    = malloc(buf_sz);
    if (!buf) return NULL;

    /* Step 1 — resolve name → hash */
    char path[256];
    snprintf(path, sizeof(path), "/data/get?name=%s", name);
    int n = http_get(sentient->ip, sentient->port, path, buf, (int)buf_sz);
    if (n <= 0) {
        fprintf(stderr, "[%s] fetch: GET %s failed\n", g_agent_name, path);
        free(buf);
        return NULL;
    }

    char hash[65] = {0};
    const char *p = strstr(buf, "\"hash\"");
    if (p) {
        p = strchr(p + 6, '"');
        if (p) {
            p++;
            size_t hi = 0;
            while (*p && *p != '"' && hi < sizeof(hash) - 1)
                hash[hi++] = *p++;
        }
    }
    if (!hash[0]) { free(buf); return NULL; }

    /* Step 2 — check cache; skip fetch if we already hold this hash */
    DataCacheEntry *existing = cache_find_by_name(name);
    if (existing && existing->valid && strcmp(existing->hash, hash) == 0) {
        existing->last_used = time(NULL);
        free(buf);
        return existing;
    }

    /* Step 3 — fetch object bytes */
    snprintf(path, sizeof(path), "/objects/%s", hash);
    n = http_get(sentient->ip, sentient->port, path, buf, (int)buf_sz);
    if (n <= 0) { free(buf); return NULL; }

    /* Step 4 — extract content; take ownership of heap buffer */
    char  *content = nebula_extract_content((const uint8_t *)buf, (size_t)n);
    size_t content_sz;
    if (content) {
        content_sz = strlen(content);
        free(buf);
    } else {
        content    = buf;
        content_sz = (size_t)n;
        content[n] = '\0';
    }

    /* Step 5 — if content is a manifest, assemble shards or keep raw for streaming */
    if (content && strncmp(content, "{\"type\":\"manifest\"", 18) == 0) {
        /* Check total_floats to decide between assembly (Phase 2) and streaming (Phase 3B) */
        int total_floats = 0;
        const char *tfp = strstr(content, "\"total_floats\":");
        if (tfp) total_floats = atoi(tfp + 15);

        if (total_floats > 0 && total_floats <= STREAM_THRESHOLD) {
            /* Small enough: assemble full NML data string in memory */
            printf("[%s] manifest detected for '%s' — assembling %d floats\n",
                   g_agent_name, name, total_floats);
            char *assembled = assemble_from_manifest(content);
            free(content);
            if (!assembled) {
                fprintf(stderr, "[%s] failed to assemble manifest for '%s'\n",
                        g_agent_name, name);
                return NULL;
            }
            content    = assembled;
            content_sz = strlen(assembled);
        } else {
            /* Too large: keep raw manifest JSON in cache for streaming execution */
            printf("[%s] manifest detected for '%s' — %d floats (streaming mode)\n",
                   g_agent_name, name, total_floats);
            /* content_sz already set above; content is the raw manifest JSON */
        }
    }

    printf("[%s] fetched '%s' hash=%s (%zu bytes)\n",
           g_agent_name, name, hash, content_sz);
    return cache_store(name, hash, content, content_sz);
}

/* ── MQTT data-lifecycle handlers ────────────────────────────────────── */

static void handle_data_ready(const char *json)
{
    char name[64] = {0}, hash[17] = {0};
    json_str(json, "name", name, sizeof(name));
    json_str(json, "hash", hash, sizeof(hash));
    if (!name[0]) return;

    /* Skip if we already hold this exact version */
    DataCacheEntry *e = cache_find_by_name(name);
    if (e && e->valid && strcmp(e->hash, hash) == 0) return;

    /* Evict stale version before pre-fetching */
    if (e && e->valid) {
        printf("[%s] invalidating stale cache for '%s' (old=%s new=%s)\n",
               g_agent_name, name, e->hash, hash);
        cache_free_entry(e);
    }

    /* Only pre-fetch datasets this worker cares about */
    if (!g_data_name || strcmp(name, g_data_name) != 0) return;

    printf("[%s] data/ready: pre-fetching '%s' hash=%s\n",
           g_agent_name, name, hash);
    fetch_and_cache(name);
}

static void handle_data_stale(const char *json)
{
    char hash[17] = {0};
    json_str(json, "hash", hash, sizeof(hash));
    if (!hash[0]) return;

    DataCacheEntry *e = cache_find_by_hash(hash);
    if (e && e->valid) {
        printf("[%s] data/stale: evicting '%s' (hash=%s) — will re-fetch\n",
               g_agent_name, e->name, hash);
        cache_free_entry(e);
        /* Leave name/hash in slot so next program triggers a fresh fetch */
    }
}

static void handle_data_reject(const char *json)
{
    char hash[17] = {0};
    json_str(json, "hash", hash, sizeof(hash));
    if (!hash[0]) return;

    DataCacheEntry *e = cache_find_by_hash(hash);
    if (e) {
        printf("[%s] data/reject: evicting '%s' (hash=%s) — will not re-fetch\n",
               g_agent_name, e->name, hash);
        cache_free_entry(e);
        e->rejected = 1;
    }
}

/* ── Transport-agnostic send helpers ─────────────────────────────────── */

static void send_result(const char *phash, const char *score_str)
{
    if (g_use_mqtt) {
        char result_payload[64];
        snprintf(result_payload, sizeof(result_payload), "%s:%s", phash, score_str);
        uint8_t buf[256];
        int n = msg_encode(buf, sizeof(buf), MSG_RESULT,
                           g_agent_name, g_agent_port, result_payload);
        if (n > 0)
            mqtt_transport_publish(&g_mqtt, MSG_RESULT, buf, (size_t)n);
    } else {
        report_send_udp(&g_udp, g_agent_name, g_agent_port, phash, score_str);
    }
    result_record(phash, score_str);
}

static void send_msg(int type, const char *payload)
{
    uint8_t buf[256];
    int n = msg_encode(buf, sizeof(buf), type, g_agent_name, g_agent_port, payload);
    if (n <= 0) return;
    if (g_use_mqtt)
        mqtt_transport_publish(&g_mqtt, type, buf, (size_t)n);
    else
        udp_send(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                 buf, (size_t)n);
}

/* ── Message handlers ────────────────────────────────────────────────── */

/*
 * Scan program text for all @label references and verify each exists in
 * the data string.  Returns 1 if all keys are present, 0 if any are missing
 * (and logs the missing key names).
 */
static int check_data_keys(const char *program, const char *data,
                            const char *agent_name)
{
    if (!data || !*data) return 1;  /* no data file — let exec handle it */

    int all_ok = 1;
    const char *p = program;
    while ((p = strchr(p, '@')) != NULL) {
        /* Find the start of this line to check the instruction */
        const char *line = p;
        while (line > program && *(line - 1) != '\n') line--;
        /* Skip leading whitespace */
        while (*line == ' ' || *line == '\t') line++;
        /* ST writes to named memory — the key is an output, not a required input */
        if (strncmp(line, "ST", 2) == 0 && (line[2] == ' ' || line[2] == '\t')) {
            p++;
            continue;
        }

        p++;  /* skip '@' */
        /* Extract label: alphanumeric + underscore */
        const char *start = p;
        while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_'))
            p++;
        size_t len = (size_t)(p - start);
        if (len == 0) continue;

        char key[64];
        if (len >= sizeof(key)) len = sizeof(key) - 1;
        memcpy(key, start, len);
        key[len] = '\0';

        /* Check for "@key " or "@key\n" or "@key" at end-of-string in data */
        char needle[66];
        snprintf(needle, sizeof(needle), "@%s", key);
        if (!strstr(data, needle)) {
            fprintf(stderr, "[%s] missing data key: @%s\n", agent_name, key);
            all_ok = 0;
        }
    }
    return all_ok;
}

/*
 * Streaming execution for datasets above STREAM_THRESHOLD.
 * manifest_json — raw manifest JSON from the data cache.
 * program_body  — unsigned NML program text.
 * score_str     — output buffer (size >= 32).
 *
 * Iterates over all shards, running one TNET pass per shard per epoch.
 * Returns 0 on success, -1 on error.
 */
static int execute_streaming(const char *program_body,
                              const char *manifest_json,
                              char *score_str, size_t score_sz)
{
    /* Parse manifest fields */
    char cust_host[128] = "127.0.0.1";
    char ds_name[64]    = "training_data";
    int  total_floats   = 0, features = 1, shard_float_count = 8192;
    uint16_t cust_port  = 9004;

    json_str(manifest_json, "custodian_host", cust_host, sizeof(cust_host));
    json_str(manifest_json, "name",           ds_name,   sizeof(ds_name));
    {
        const char *p;
        p = strstr(manifest_json, "\"total_floats\":");   if (p) total_floats       = atoi(p + 15);
        p = strstr(manifest_json, "\"features\":");       if (p) features            = atoi(p + 11);
        p = strstr(manifest_json, "\"shard_float_count\":"); if (p) shard_float_count = atoi(p + 20);
        p = strstr(manifest_json, "\"custodian_port\":");  if (p) cust_port          = (uint16_t)atoi(p + 17);
    }
    if (total_floats <= 0 || features < 1) return -1;
    if (shard_float_count <= 0) shard_float_count = 8192;

    /* Extract epochs from TNET immediate in the program text */
    int epochs = 1;
    const char *tp = strstr(program_body, "TNET ");
    if (!tp) tp = strstr(program_body, "\xe2\xa5\x81 ");  /* ⥁ */
    if (tp) {
        const char *ep = strchr(tp, '#');
        if (ep) epochs = atoi(ep + 1);
    }
    if (epochs < 1) epochs = 1;

    printf("[%s] streaming execution: %d floats, %d features, %d epochs\n",
           g_agent_name, total_floats, features, epochs);

    /* Compile program and prepare stateful VM */
    NmlExecCtx *ctx = nml_exec_create(program_body);
    if (!ctx) {
        fprintf(stderr, "[%s] streaming: failed to create exec context\n",
                g_agent_name);
        return -1;
    }

    /* Shard fetch buffer */
    char  *shard_buf = (char *)malloc(SHARD_BUF_SZ);
    if (!shard_buf) { nml_exec_destroy(ctx); return -1; }

    int samples_per_shard = shard_float_count / features;
    if (samples_per_shard < 1) samples_per_shard = 1;

    for (int epoch = 0; epoch < epochs; epoch++) {
        const char *sp = strstr(manifest_json, "\"shards\":[");
        if (!sp) break;
        sp += 10;

        while (*sp) {
            while (*sp == ' ' || *sp == ',') sp++;
            if (*sp == ']' || *sp == '\0') break;
            if (*sp != '"') { sp++; continue; }
            sp++;
            char shash[17] = {0};
            int hi = 0;
            while (*sp && *sp != '"' && hi < 16) shash[hi++] = *sp++;
            if (*sp == '"') sp++;
            if (hi != 16) continue;

            /* Fetch full shard */
            char spath[64];
            snprintf(spath, sizeof(spath), "/objects/%s", shash);
            int sn = http_get(cust_host, cust_port, spath, shard_buf, SHARD_BUF_SZ);
            if (sn <= 0) {
                fprintf(stderr, "[%s] streaming: failed to fetch shard %s\n",
                        g_agent_name, shash);
                continue;
            }

            /* Count floats from NebulaDisk content_len */
            size_t raw_bytes   = nebula_content_len((const uint8_t *)shard_buf, (size_t)sn);
            int    float_count = (int)(raw_bytes / sizeof(float));
            if (float_count <= 0) continue;

            /* Extract raw float bytes */
            char *raw = nebula_extract_content((const uint8_t *)shard_buf, (size_t)sn);
            if (!raw) continue;

            /* Build NML data string for this shard */
            int shard_samples = float_count / features;
            if (shard_samples < 1) { free(raw); continue; }

            size_t dsz = 128 + (size_t)float_count * 14;
            char  *data_str = (char *)malloc(dsz);
            size_t lsz = 128 + (size_t)shard_samples * 14;
            char  *lbl_str  = (char *)malloc(lsz);
            if (!data_str || !lbl_str) {
                free(raw); free(data_str); free(lbl_str); continue;
            }

            /* First (features-1) columns per row → training_data */
            /* Last column → training_labels */
            int input_cols = features - 1;
            if (input_cols < 1) input_cols = features;

            int dp = snprintf(data_str, dsz,
                "@%s shape=%d,%d dtype=f32 data=", ds_name, shard_samples, input_cols);
            int lp = snprintf(lbl_str, lsz,
                "@training_labels shape=%d,1 dtype=f32 data=", shard_samples);

            const float *fptr = (const float *)raw;
            for (int s = 0; s < shard_samples; s++) {
                for (int c = 0; c < input_cols; c++) {
                    if (s > 0 || c > 0) { data_str[dp++] = ','; }
                    dp += snprintf(data_str + dp, dsz - (size_t)dp, "%.6g",
                                   fptr[s * features + c]);
                }
                if (s > 0) { lbl_str[lp++] = ','; }
                lp += snprintf(lbl_str + lp, lsz - (size_t)lp, "%.6g",
                               fptr[s * features + (features - 1)]);
            }
            free(raw);

            nml_exec_load_shard(ctx, data_str, lbl_str);
            free(data_str);
            free(lbl_str);
            nml_exec_run_pass(ctx, 0.01f);
        }
    }

    free(shard_buf);

    int rc = nml_exec_score(ctx, score_str, score_sz);
    nml_exec_destroy(ctx);
    return rc;
}

static void handle_program(const char *compact_payload,
                            const char *peer_name,
                            int require_signed)
{
    static char program[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact_payload, program, sizeof(program)) < 0) {
        fprintf(stderr, "[%s] compact decode overflow from %s\n",
                g_agent_name, peer_name);
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
                    g_agent_name, peer_name);
            return;
        }
        if (vrc == 0) {
            printf("[%s] verified — signed by '%s'\n", g_agent_name, signer);
            is_signed = 1;
        }
    } else if (require_signed) {
        fprintf(stderr, "[%s] REJECTED unsigned program from %s (--require-signed)\n",
                g_agent_name, peer_name);
        return;
    }

    /* Look up data in cache; lazy-fetch if missing */
    const char *data = NULL;
    if (g_data_name && *g_data_name) {
        DataCacheEntry *e = cache_find_by_name(g_data_name);

        if (e && e->rejected) {
            fprintf(stderr, "[%s] skipping — data '%s' was rejected\n",
                    g_agent_name, g_data_name);
            return;
        }

        if (!e || !e->valid)
            e = fetch_and_cache(g_data_name);

        if (e && e->valid) {
            e->last_used = time(NULL);
            data = e->ptr;
        }
    }

    if (!data) {
        fprintf(stderr, "[%s] skipping program — no data available\n", g_agent_name);
        return;
    }

    char phash[17];
    crypto_program_hash(body, phash);

    printf("[%s] executing %s (signed=%d) from %s\n",
           g_agent_name, phash, is_signed, peer_name);

    char score_str[32];
    int rc;

    /* Detect manifest JSON in the data cache — use streaming execution path */
    if (data && strncmp(data, "{\"type\":\"manifest\"", 18) == 0) {
        rc = execute_streaming(body, data, score_str, sizeof(score_str));
    } else {
        /* Pre-flight: verify all @key references exist in our data */
        if (!check_data_keys(body, data, g_agent_name)) {
            fprintf(stderr, "[%s] skipping %s — data keys missing (see above)\n",
                    g_agent_name, phash);
            return;
        }
        rc = nml_exec_run(body, data, score_str, sizeof(score_str));
    }

    if (rc == 0) {
        printf("[%s] score=%s hash=%s\n", g_agent_name, score_str, phash);
        send_result(phash, score_str);
    } else if (rc == -2) {
        fprintf(stderr, "[%s] no score key in result (hash=%s)\n",
                g_agent_name, phash);
    } else {
        fprintf(stderr, "[%s] execution failed (hash=%s)\n", g_agent_name, phash);
    }
}

static void handle_result(const char *payload, const char *peer_name)
{
    char phash[17] = {0};
    float score = 0.0f;
    if (sscanf(payload, "%16[^:]:%f", phash, &score) == 2) {
        int r = vote_add(&g_votes, phash, peer_name, score, 1, time(NULL));
        if (r == 1) {
            float mean;
            vote_get_result(&g_votes, phash, &mean);
            printf("[%s] COMMIT hash=%s mean_score=%.6f\n",
                   g_agent_name, phash, mean);
        }
    } else {
        printf("[%s] result from %s: %s\n", g_agent_name, peer_name, payload);
    }
}

static void dispatch(int type, const char *peer_name, const char *sender_ip,
                     uint16_t peer_port, const char *payload,
                     int require_signed, time_t now)
{
    if (strcmp(peer_name, g_agent_name) == 0) return;

    switch (type) {
    case MSG_ANNOUNCE:
        peer_upsert(&g_peers, peer_name, sender_ip, peer_port, payload, now);
        printf("[%s] peer: %s ip=%s (port %u)\n",
               g_agent_name, peer_name, sender_ip[0] ? sender_ip : "?", peer_port);
        break;

    case MSG_HEARTBEAT:
        peer_upsert(&g_peers, peer_name, sender_ip, peer_port, payload, now);
        break;

    case MSG_PROGRAM: {
        PeerEntry *sender = peer_get(&g_peers, peer_name);
        if (sender && sender->quarantined) {
            fprintf(stderr, "[%s] IGNORED program from quarantined peer %s\n",
                    g_agent_name, peer_name);
            break;
        }
        handle_program(payload, peer_name, require_signed);
        break;
    }

    case MSG_RESULT:
        handle_result(payload, peer_name);
        break;

    case MSG_ENFORCE: {
        char type_str[8] = {0}, target[64] = {0}, reason[128] = {0};
        const char *p = payload;
        size_t tl = strcspn(p, "|");
        if (tl < sizeof(type_str)) { memcpy(type_str, p, tl); p += tl; }
        if (*p == '|') p++;
        size_t nl = strcspn(p, "|");
        if (nl < sizeof(target)) { memcpy(target, p, nl); p += nl; }
        if (*p == '|') { p++; snprintf(reason, sizeof(reason), "%s", p); }

        if (strncmp(type_str, "Q", 1) == 0) {
            peer_quarantine(&g_peers, target, reason);
            printf("[%s] enforced quarantine: %s\n", g_agent_name, target);
        } else if (strncmp(type_str, "U", 1) == 0) {
            PeerEntry *pe = peer_get(&g_peers, target);
            if (pe) { pe->quarantined = 0; pe->active = 1; }
            printf("[%s] quarantine lifted: %s\n", g_agent_name, target);
        }
        break;
    }

    default:
        break;
    }
}

/* ── HTTP server (uses shared edge/http_util) ─────────────────────────── */

static void handle_http(compat_socket_t cfd)
{
    struct timeval tv = {10, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO,
               COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO,
               COMPAT_SOCKOPT_CAST(&tv), sizeof(tv));

    static char req[16 * 1024];
    int n = http_recv_full(cfd, req, sizeof(req), 64 * 1024);
    if (n <= 0) { compat_close_socket(cfd); return; }

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        compat_close_socket(cfd); return;
    }

    /* GET / — role landing page */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send_html(cfd, (const char *)ui_html, (size_t)ui_html_len);
        compat_close_socket(cfd); return;
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        int cached = 0;
        for (int i = 0; i < DATA_CACHE_MAX; i++)
            if (g_cache[i].valid) cached++;
        char body[256];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"peers\":%d,\"results\":%d,\"cache\":%d}",
            g_agent_name, g_peers.count, g_results_count, cached);
        http_send_json(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[8192];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send_json(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /results — most-recent-first JSON array of (phash, score, ts) */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/results") == 0) {
        char body[8192];
        int pos = snprintf(body, sizeof(body), "[");
        int emitted = 0;
        /* Walk newest -> oldest: head is next write, so (head-1) is newest */
        for (int i = 0; i < g_results_count; i++) {
            int idx = (g_results_head - 1 - i + RESULT_HISTORY_MAX) % RESULT_HISTORY_MAX;
            const ResultEntry *e = &g_results[idx];
            if (emitted > 0 && (size_t)pos < sizeof(body))
                pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            if ((size_t)pos < sizeof(body)) {
                pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                    "{\"phash\":\"%s\",\"score\":\"%s\",\"ts\":%ld}",
                    e->phash, e->score, (long)e->ts);
            }
            emitted++;
        }
        if ((size_t)pos < sizeof(body))
            snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send_json(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /data/cache — current local dataset cache */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/data/cache") == 0) {
        char body[4096];
        int pos = snprintf(body, sizeof(body), "[");
        int first = 1;
        for (int i = 0; i < DATA_CACHE_MAX; i++) {
            const DataCacheEntry *e = &g_cache[i];
            if (e->name[0] == '\0' && !e->valid && !e->rejected) continue;
            if (!first && (size_t)pos < sizeof(body))
                pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            first = 0;
            if ((size_t)pos < sizeof(body)) {
                pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                    "{\"name\":\"%s\",\"hash\":\"%s\","
                     "\"size\":%zu,\"is_mmap\":%s,"
                     "\"valid\":%s,\"rejected\":%s,\"last_used\":%ld}",
                    e->name, e->hash, e->size,
                    e->is_mmap  ? "true" : "false",
                    e->valid    ? "true" : "false",
                    e->rejected ? "true" : "false",
                    (long)e->last_used);
            }
        }
        if ((size_t)pos < sizeof(body))
            snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send_json(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        http_send_json(cfd, 200, "{}");
        compat_close_socket(cfd); return;
    }

    http_send_json(cfd, 404, "{\"error\":\"not found\"}");
    compat_close_socket(cfd);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    compat_winsock_init();

    const char *data_path    = NULL;
    const char *data_name    = NULL;
    int         require_signed = 0;
    char        broker_host[128] = "127.0.0.1";
    uint16_t    broker_port      = 1883;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            g_agent_name = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_agent_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_path = argv[++i];
        } else if (strcmp(argv[i], "--data-name") == 0 && i + 1 < argc) {
            data_name = argv[++i];
        } else if (strcmp(argv[i], "--require-signed") == 0) {
            require_signed = 1;
        } else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc) {
            snprintf(broker_host, sizeof(broker_host), "%s", argv[++i]);
            g_use_mqtt = 1;
        } else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc) {
            broker_port = (uint16_t)atoi(argv[++i]);
            g_use_mqtt = 1;
        } else {
            fprintf(stderr,
                "Usage: %s [--name NAME] [--port PORT] [--data FILE]\n"
                "          [--data-name NAME] [--require-signed]\n"
                "          [--broker HOST] [--broker-port PORT]\n",
                argv[0]);
            return 1;
        }
    }

    /* Load --data FILE into cache slot as "__local__" */
    if (data_path) {
        char *file_data = read_file(data_path);
        if (!file_data) {
            fprintf(stderr, "[%s] WARNING: could not read data file %s\n",
                    g_agent_name, data_path);
        } else {
            cache_store("__local__", "", file_data, strlen(file_data));
            g_data_name = "__local__";
            printf("[%s] loaded data from %s\n", g_agent_name, data_path);
        }
    } else if (data_name) {
        g_data_name = data_name;
    }

    identity_init(g_agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);
    printf("[%s] identity: %s\n", g_agent_name, g_identity_payload);

    peer_table_init(&g_peers);
    vote_table_init(&g_votes);

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
        printf("[%s] connected to broker %s:%u (port=%u require_signed=%d)\n",
               g_agent_name, broker_host, broker_port,
               g_agent_port, require_signed);
    } else {
        if (udp_init(&g_udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT) < 0) {
            fprintf(stderr, "[%s] failed to init UDP\n", g_agent_name);
            return 1;
        }
        printf("[%s] joined %s:%u (port=%u require_signed=%d)\n",
               g_agent_name, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
               g_agent_port, require_signed);
        send_msg(MSG_ANNOUNCE, g_identity_payload);
    }

    if (g_data_name && strcmp(g_data_name, "__local__") != 0)
        printf("[%s] will fetch data '%s' from sentient on first program "
               "(or on nml/data/ready)\n", g_agent_name, g_data_name);

    /* HTTP API server */
    g_http_fd = http_listen(g_agent_port);
    if (g_http_fd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[%s] failed to bind HTTP on port %u\n",
                g_agent_name, g_agent_port);
        if (g_use_mqtt) mqtt_transport_close(&g_mqtt);
        else            udp_close(&g_udp);
        return 1;
    }
    printf("[%s] HTTP API on port %u\n", g_agent_name, g_agent_port);

    static uint8_t in_buf[65536];
    static char    peer_name[64];
    static char    payload[NML_MAX_PROGRAM_LEN + 1];
    static char    sender_ip[46];
    static char    topic[128];

    time_t last_heartbeat = time(NULL);
    time_t last_sweep     = last_heartbeat;

    while (g_running) {
        time_t now = time(NULL);

        /* HTTP accept with 1 s timeout — HTTP has to share the loop with the
         * transport, so we use select() here and run the transport in
         * non-blocking mode below. */
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(g_http_fd, &rfds);
            struct timeval htv = {1, 0};
            int sel = select(COMPAT_SELECT_NFDS(g_http_fd),
                             &rfds, NULL, NULL, &htv);
            if (sel > 0 && FD_ISSET(g_http_fd, &rfds)) {
                compat_socket_t cfd = accept(g_http_fd, NULL, NULL);
                if (cfd != COMPAT_INVALID_SOCKET) handle_http(cfd);
            }
        }

        if (g_use_mqtt) {
            mqtt_transport_sync(&g_mqtt, 0);

            int pkt_len;
            while ((pkt_len = mqtt_transport_recv_ex(&g_mqtt, in_buf,
                                                     sizeof(in_buf) - 1,
                                                     sender_ip, topic)) > 0) {
                /* Plain JSON data-lifecycle topics */
                if (strcmp(topic, "nml/data/ready") == 0) {
                    in_buf[pkt_len] = '\0';
                    handle_data_ready((const char *)in_buf);
                    continue;
                }
                if (strcmp(topic, "nml/data/stale") == 0) {
                    in_buf[pkt_len] = '\0';
                    handle_data_stale((const char *)in_buf);
                    continue;
                }
                if (strcmp(topic, "nml/data/reject") == 0) {
                    in_buf[pkt_len] = '\0';
                    handle_data_reject((const char *)in_buf);
                    continue;
                }

                /* NML wire-format messages */
                int      type;
                uint16_t peer_port;
                if (msg_parse(in_buf, (size_t)pkt_len,
                              &type, peer_name, sizeof(peer_name),
                              &peer_port, payload, sizeof(payload)) < 0)
                    continue;
                dispatch(type, peer_name, sender_ip, peer_port, payload,
                         require_signed, now);
            }
        } else {
            sender_ip[0] = '\0';
            /* Non-blocking — HTTP select above paced the loop. */
            int received = udp_recv(&g_udp, in_buf, sizeof(in_buf), 0, sender_ip);
            if (received > 0) {
                int      type;
                uint16_t peer_port;
                if (msg_parse(in_buf, (size_t)received,
                              &type, peer_name, sizeof(peer_name),
                              &peer_port, payload, sizeof(payload)) >= 0) {
                    dispatch(type, peer_name, sender_ip, peer_port, payload,
                             require_signed, now);
                }
            }
        }

        if (now - last_heartbeat >= HEARTBEAT_INTERVAL) {
            send_msg(MSG_HEARTBEAT, g_identity_payload);
            last_heartbeat = now;
        }

        if (now - last_sweep >= 30) {
            peer_sweep(&g_peers, now, HEARTBEAT_INTERVAL * 6);
            vote_expire(&g_votes, now, 120);
            last_sweep = now;
        }
    }

    printf("\n[%s] shutting down\n", g_agent_name);

    if (g_http_fd != COMPAT_INVALID_SOCKET)
        compat_close_socket(g_http_fd);

    /* Release cache */
    for (int i = 0; i < DATA_CACHE_MAX; i++)
        cache_free_entry(&g_cache[i]);

    if (g_use_mqtt)
        mqtt_transport_close(&g_mqtt);
    else
        udp_close(&g_udp);

    compat_winsock_cleanup();
    return 0;
}
