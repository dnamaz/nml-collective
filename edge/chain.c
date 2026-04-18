/*
 * chain.c — Per-agent hash-linked transaction chain.
 *
 * See chain.h for the on-disk record layout.
 */

#define _POSIX_C_SOURCE 200809L

#include "chain.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define NML_CRYPTO
#include "../../nml/runtime/nml_crypto.h"

#ifdef COMPAT_WINDOWS
  #include <io.h>
  #define chain_fsync(f) _commit(_fileno(f))
#else
  #include <unistd.h>
  #define chain_fsync(f) fsync(fileno(f))
#endif

/* ── Endian helpers ───────────────────────────────────────────────────── */

static void u64_to_be(uint64_t v, uint8_t out[8])
{
    out[0] = (uint8_t)((v >> 56) & 0xFF);
    out[1] = (uint8_t)((v >> 48) & 0xFF);
    out[2] = (uint8_t)((v >> 40) & 0xFF);
    out[3] = (uint8_t)((v >> 32) & 0xFF);
    out[4] = (uint8_t)((v >> 24) & 0xFF);
    out[5] = (uint8_t)((v >> 16) & 0xFF);
    out[6] = (uint8_t)((v >>  8) & 0xFF);
    out[7] = (uint8_t)((v      ) & 0xFF);
}

static uint64_t be_to_u64(const uint8_t in[8])
{
    return ((uint64_t)in[0] << 56) | ((uint64_t)in[1] << 48) |
           ((uint64_t)in[2] << 40) | ((uint64_t)in[3] << 32) |
           ((uint64_t)in[4] << 24) | ((uint64_t)in[5] << 16) |
           ((uint64_t)in[6] <<  8) | ((uint64_t)in[7]      );
}

static void u16_to_be(uint16_t v, uint8_t out[2])
{
    out[0] = (uint8_t)((v >> 8) & 0xFF);
    out[1] = (uint8_t)((v     ) & 0xFF);
}

static uint16_t be_to_u16(const uint8_t in[2])
{
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

/* ── Hash computation ─────────────────────────────────────────────────── */

/*
 * hash = SHA-256(prev_hash(8) || tx_id_BE(8) || timestamp_BE(8)
 *                || tx_type(1) || payload_len_BE(2) || payload)[0:8]
 */
static void compute_record_hash(const uint8_t prev_hash[CHAIN_HASH_LEN],
                                uint64_t tx_id, uint64_t timestamp,
                                uint8_t tx_type, uint16_t payload_len,
                                const uint8_t *payload,
                                uint8_t out_hash[CHAIN_HASH_LEN])
{
    /* Build the hashed region into a single buffer. */
    uint8_t hdr[CHAIN_HASH_LEN + 8 + 8 + 1 + 2];
    uint8_t *p = hdr;
    memcpy(p, prev_hash, CHAIN_HASH_LEN); p += CHAIN_HASH_LEN;
    u64_to_be(tx_id,      p); p += 8;
    u64_to_be(timestamp,  p); p += 8;
    *p++ = tx_type;
    u16_to_be(payload_len, p); p += 2;

    /* Two-part hash: header region, then payload (variable length). */
    uint8_t full[32];
    if (payload_len == 0) {
        sha256(hdr, sizeof(hdr), full);
    } else {
        /* Concat into a temp buffer; payload_len is bounded by
         * CHAIN_MAX_PAYLOAD so this is safe on the stack. */
        uint8_t combined[sizeof(hdr) + CHAIN_MAX_PAYLOAD];
        memcpy(combined, hdr, sizeof(hdr));
        memcpy(combined + sizeof(hdr), payload, payload_len);
        sha256(combined, sizeof(hdr) + payload_len, full);
    }
    memcpy(out_hash, full, CHAIN_HASH_LEN);
}

/* ── Record I/O ───────────────────────────────────────────────────────── */

/*
 * Read one record from the current file cursor.
 * Returns 0 on success, 1 on clean EOF before any byte, -1 on error.
 * On success, *rec is populated and the cursor sits right after the payload.
 * If payload_out is non-NULL, the payload is copied into it; caller must
 * ensure payload_buf_sz >= CHAIN_MAX_PAYLOAD.
 */
static int read_record(FILE *f, ChainRecord *rec,
                       uint8_t *payload_out, size_t payload_buf_sz)
{
    long start_off = ftell(f);
    if (start_off < 0) return -1;

    uint8_t prefix[CHAIN_PREFIX_LEN];
    size_t n = fread(prefix, 1, sizeof(prefix), f);
    if (n == 0) return 1;                    /* clean EOF */
    if (n != sizeof(prefix)) return -1;      /* truncated prefix */

    rec->file_offset = start_off;
    rec->tx_id       = be_to_u64(prefix +  0);
    rec->timestamp   = be_to_u64(prefix +  8);
    rec->tx_type     = prefix[16];
    rec->payload_len = be_to_u16(prefix + 17);
    memcpy(rec->prev_hash, prefix + 19, CHAIN_HASH_LEN);
    memcpy(rec->hash,      prefix + 27, CHAIN_HASH_LEN);

    if (rec->payload_len > CHAIN_MAX_PAYLOAD) return -1;

    if (rec->payload_len > 0) {
        if (payload_out) {
            if (payload_buf_sz < rec->payload_len) return -1;
            if (fread(payload_out, 1, rec->payload_len, f)
                != rec->payload_len) return -1;
        } else {
            /* Skip payload */
            if (fseek(f, (long)rec->payload_len, SEEK_CUR) != 0) return -1;
        }
    }
    return 0;
}

/* ── mkdir -p helper for <dir>/agents/<name> ──────────────────────────── */

static int ensure_chain_dirs(const char *dir, const char *agent_name,
                             char *out_path, size_t out_sz)
{
    char agents_dir[400];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agents", dir);
    if (compat_mkdir(agents_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[chain] mkdir %s: %s\n",
                agents_dir, strerror(errno));
        return -1;
    }

    char name_dir[450];
    snprintf(name_dir, sizeof(name_dir), "%s/agents/%s", dir, agent_name);
    if (compat_mkdir(name_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[chain] mkdir %s: %s\n",
                name_dir, strerror(errno));
        return -1;
    }

    snprintf(out_path, out_sz, "%s/chain.binlog", name_dir);
    return 0;
}

/* ── chain_open ───────────────────────────────────────────────────────── */

int chain_open(const char *dir, const char *agent_name, Chain *c)
{
    if (!dir || !agent_name || !c) return -1;
    memset(c, 0, sizeof(*c));

    if (ensure_chain_dirs(dir, agent_name, c->path, sizeof(c->path)) != 0)
        return -1;

    /* Try opening existing chain for read+write. */
    c->f = fopen(c->path, "rb+");
    if (c->f) {
        /* Walk to validate and recover last_hash + next_tx_id. */
        uint64_t bad = 0;
        int count = chain_verify(c, &bad);
        if (count < 0) {
            fprintf(stderr,
                    "[chain] integrity failure in %s at tx_id=%llu\n",
                    c->path, (unsigned long long)bad);
            fclose(c->f);
            c->f = NULL;
            return -1;
        }

        /* chain_verify leaves the cursor positioned after the last record;
         * next_tx_id and last_hash are populated as a side effect of its
         * scan. Re-seek explicitly to be safe. */
        if (fseek(c->f, 0, SEEK_END) != 0) {
            fclose(c->f); c->f = NULL; return -1;
        }
        c->verified = 1;
        return 0;
    }

    /* Fresh chain: create file and write AGENT_JOIN(agent_name). */
    c->f = fopen(c->path, "wb+");
    if (!c->f) {
        fprintf(stderr, "[chain] fopen %s: %s\n",
                c->path, strerror(errno));
        return -1;
    }
    c->next_tx_id = 0;
    memset(c->last_hash, 0, CHAIN_HASH_LEN);
    c->verified = 1;

    size_t name_len = strlen(agent_name);
    if (name_len > CHAIN_MAX_PAYLOAD) name_len = CHAIN_MAX_PAYLOAD;
    return chain_append(c, CHAIN_TX_AGENT_JOIN,
                        agent_name, name_len);
}

/* ── chain_close ──────────────────────────────────────────────────────── */

void chain_close(Chain *c)
{
    if (!c) return;
    if (c->f) { fclose(c->f); c->f = NULL; }
}

/* ── chain_append ─────────────────────────────────────────────────────── */

int chain_append(Chain *c, uint8_t tx_type,
                 const void *payload, size_t payload_len)
{
    if (!c || !c->f) return -1;
    if (payload_len > CHAIN_MAX_PAYLOAD) return -1;
    if (payload_len > 0 && payload == NULL) return -1;

    uint64_t tx_id = c->next_tx_id;
    uint64_t ts    = (uint64_t)time(NULL);
    uint16_t plen  = (uint16_t)payload_len;

    uint8_t hash[CHAIN_HASH_LEN];
    compute_record_hash(c->last_hash, tx_id, ts, tx_type, plen,
                        (const uint8_t *)payload, hash);

    /* Serialize prefix */
    uint8_t prefix[CHAIN_PREFIX_LEN];
    u64_to_be(tx_id, prefix +  0);
    u64_to_be(ts,    prefix +  8);
    prefix[16] = tx_type;
    u16_to_be(plen, prefix + 17);
    memcpy(prefix + 19, c->last_hash, CHAIN_HASH_LEN);
    memcpy(prefix + 27, hash,         CHAIN_HASH_LEN);

    if (fseek(c->f, 0, SEEK_END) != 0) return -1;
    if (fwrite(prefix, 1, sizeof(prefix), c->f) != sizeof(prefix)) return -1;
    if (plen > 0 &&
        fwrite(payload, 1, plen, c->f) != plen) return -1;

    if (fflush(c->f) != 0) return -1;
    chain_fsync(c->f);   /* best-effort durability */

    memcpy(c->last_hash, hash, CHAIN_HASH_LEN);
    c->next_tx_id = tx_id + 1;
    return 0;
}

/* ── chain_verify ─────────────────────────────────────────────────────── */

/*
 * chain_verify() takes (const Chain *) in the public API but mutates the
 * caller's Chain to refresh next_tx_id and last_hash — treating verify as
 * the canonical rescan. Callers pass a non-const pointer in practice; the
 * const qualifier documents that on-disk state isn't changed.
 */
int chain_verify(const Chain *c_const, uint64_t *bad_tx_id)
{
    Chain *c = (Chain *)c_const;
    if (!c || !c->f) return -1;

    if (fseek(c->f, 0, SEEK_SET) != 0) return -1;

    uint8_t  prev_hash[CHAIN_HASH_LEN] = {0};
    uint64_t expected_tx_id = 0;
    int      count = 0;
    uint8_t  payload[CHAIN_MAX_PAYLOAD];

    for (;;) {
        ChainRecord rec;
        int rc = read_record(c->f, &rec, payload, sizeof(payload));
        if (rc == 1) break;                  /* EOF */
        if (rc < 0) {
            if (bad_tx_id) *bad_tx_id = expected_tx_id;
            return -1;
        }

        if (rec.tx_id != expected_tx_id) {
            if (bad_tx_id) *bad_tx_id = rec.tx_id;
            return -1;
        }
        if (memcmp(rec.prev_hash, prev_hash, CHAIN_HASH_LEN) != 0) {
            if (bad_tx_id) *bad_tx_id = rec.tx_id;
            return -1;
        }

        uint8_t want[CHAIN_HASH_LEN];
        compute_record_hash(prev_hash, rec.tx_id, rec.timestamp,
                            rec.tx_type, rec.payload_len,
                            payload, want);
        if (memcmp(want, rec.hash, CHAIN_HASH_LEN) != 0) {
            if (bad_tx_id) *bad_tx_id = rec.tx_id;
            return -1;
        }

        memcpy(prev_hash, rec.hash, CHAIN_HASH_LEN);
        expected_tx_id++;
        count++;
    }

    memcpy(c->last_hash, prev_hash, CHAIN_HASH_LEN);
    c->next_tx_id = expected_tx_id;
    return count;
}

/* ── chain_iter ───────────────────────────────────────────────────────── */

int chain_iter(const Chain *c, chain_iter_cb cb, void *ud)
{
    if (!c || !c->f || !cb) return -1;
    FILE *f = c->f;

    if (fseek(f, 0, SEEK_SET) != 0) return -1;

    uint8_t payload[CHAIN_MAX_PAYLOAD];
    for (;;) {
        ChainRecord rec;
        int rc = read_record(f, &rec, payload, sizeof(payload));
        if (rc == 1) break;
        if (rc < 0) return -1;
        cb(&rec, payload, ud);
    }
    return 0;
}
