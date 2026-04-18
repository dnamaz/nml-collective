/*
 * storage.c — Minimal content-addressed NebulaDisk object store.
 *
 * Implements the NebulaDisk binary format used by nml_storage.py.
 * No dynamic allocation; all buffers are fixed-size stack variables.
 *
 * See storage.h for API documentation.
 */

#define _POSIX_C_SOURCE 200809L

#include "storage.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

/* Pull in sha256 and hex_encode as static functions. */
#define NML_CRYPTO
#include "../../nml/runtime/nml_crypto.h"

/* NebulaDisk magic bytes */
#define NEBULA_MAGIC "\x4e\x4d\x4c\x02"  /* "NML\x02" */
#define NEBULA_MAGIC_LEN 4

/* ── Internal path helper ─────────────────────────────────────────────────── */

static void obj_path(const char *dir, const char *hash, char *out, size_t sz)
{
    /* {dir}/objects/{hash[0:2]}/{hash}.obj */
    snprintf(out, sz, "%s/objects/%.2s/%s.obj", dir, hash, hash);
}

/* ── Endian helpers ───────────────────────────────────────────────────────── */

static void u32_to_be(uint32_t v, uint8_t out[4])
{
    out[0] = (uint8_t)((v >> 24) & 0xFF);
    out[1] = (uint8_t)((v >> 16) & 0xFF);
    out[2] = (uint8_t)((v >>  8) & 0xFF);
    out[3] = (uint8_t)((v      ) & 0xFF);
}

static uint32_t be_to_u32(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] <<  8) |
           ((uint32_t)in[3]      );
}

/* ── storage_put_shaped ───────────────────────────────────────────────────── */

int storage_put_shaped(const char *dir, const char *content, size_t len,
                       int obj_type, int dtype,
                       int ndims, const uint32_t *shape,
                       const char *author, const char *name,
                       char *hash_out)
{
    (void)name; /* name is metadata only; hash is the canonical key */

    if (ndims < 0 || ndims > STORAGE_MAX_NDIMS) return -1;
    if (ndims > 0 && shape == NULL) return -1;

    /* 1. Compute SHA-256(content) */
    uint8_t full_hash[32];
    sha256((const uint8_t *)content, len, full_hash);

    /* 2. hex_encode first 8 bytes → 16-char hex hash */
    hex_encode(full_hash, 8, hash_out);
    hash_out[16] = '\0';

    /* 3. Build object file path */
    char fpath[512];
    obj_path(dir, hash_out, fpath, sizeof(fpath));

    /* 4. Dedup: if the file already exists, return 0 immediately */
    {
        FILE *chk = fopen(fpath, "rb");
        if (chk) {
            fclose(chk);
            return 0;
        }
    }

    /* 5. Create parent directories (ignore EEXIST) */
    {
        char objs_dir[512];
        snprintf(objs_dir, sizeof(objs_dir), "%s/objects", dir);
        if (compat_mkdir(objs_dir, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "[storage] mkdir %s: %s\n", objs_dir, strerror(errno));
            return -1;
        }

        char sub_dir[512];
        snprintf(sub_dir, sizeof(sub_dir), "%s/objects/%.2s", dir, hash_out);
        if (compat_mkdir(sub_dir, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "[storage] mkdir %s: %s\n", sub_dir, strerror(errno));
            return -1;
        }
    }

    /* 6. Write NebulaDisk binary */
    FILE *f = fopen(fpath, "wb");
    if (!f) {
        fprintf(stderr, "[storage] fopen %s: %s\n", fpath, strerror(errno));
        return -1;
    }

    /* magic: NML\x02 */
    fwrite(NEBULA_MAGIC, 1, NEBULA_MAGIC_LEN, f);

    /* hash_bytes: first 8 bytes of SHA-256 */
    fwrite(full_hash, 1, 8, f);

    /* obj_type: 1 byte */
    {
        uint8_t ot = (uint8_t)obj_type;
        fwrite(&ot, 1, 1, f);
    }

    /* author_len: strlen(author) clipped to 63, as 1 byte */
    size_t alen = author ? strlen(author) : 0;
    if (alen > 63) alen = 63;
    {
        uint8_t al = (uint8_t)alen;
        fwrite(&al, 1, 1, f);
    }

    /* author: author_len bytes */
    if (alen > 0) {
        fwrite(author, 1, alen, f);
    }

    /* timestamp: current time as double, written big-endian (8 bytes) */
    {
        union { double d; uint64_t u; } tu;
        tu.d = (double)time(NULL);
        uint8_t ts_bytes[8];
        ts_bytes[0] = (uint8_t)((tu.u >> 56) & 0xFF);
        ts_bytes[1] = (uint8_t)((tu.u >> 48) & 0xFF);
        ts_bytes[2] = (uint8_t)((tu.u >> 40) & 0xFF);
        ts_bytes[3] = (uint8_t)((tu.u >> 32) & 0xFF);
        ts_bytes[4] = (uint8_t)((tu.u >> 24) & 0xFF);
        ts_bytes[5] = (uint8_t)((tu.u >> 16) & 0xFF);
        ts_bytes[6] = (uint8_t)((tu.u >>  8) & 0xFF);
        ts_bytes[7] = (uint8_t)((tu.u      ) & 0xFF);
        fwrite(ts_bytes, 1, 8, f);
    }

    /* ndims: 1 byte */
    {
        uint8_t nd = (uint8_t)ndims;
        fwrite(&nd, 1, 1, f);
    }

    /* shape: ndims * 4 bytes, each uint32 big-endian */
    for (int i = 0; i < ndims; i++) {
        uint8_t dim_bytes[4];
        u32_to_be(shape[i], dim_bytes);
        fwrite(dim_bytes, 1, 4, f);
    }

    /* dtype: 1 byte */
    {
        uint8_t dt = (uint8_t)dtype;
        fwrite(&dt, 1, 1, f);
    }

    /* content_len: (uint32_t)len written big-endian (4 bytes) */
    {
        uint8_t cl_bytes[4];
        u32_to_be((uint32_t)len, cl_bytes);
        fwrite(cl_bytes, 1, 4, f);
    }

    /* content: len bytes */
    fwrite(content, 1, len, f);

    fclose(f);
    return 0;
}

/* ── storage_put (compat wrapper) ─────────────────────────────────────────── */

int storage_put(const char *dir, const char *content, size_t len,
                int obj_type, const char *author, const char *name,
                char *hash_out)
{
    return storage_put_shaped(dir, content, len, obj_type,
                              STORAGE_DTYPE_TEXT, 0, NULL,
                              author, name, hash_out);
}

/* ── Internal: parse full header from an open FILE *. ─────────────────────
 *
 * On success, *out (if non-NULL) is populated and the file cursor is left at
 * the first byte of the content payload.  Returns 0 on success, -1 on parse
 * failure or premature EOF.
 */
static int read_header(FILE *f, StorageHeader *out)
{
    uint8_t magic[NEBULA_MAGIC_LEN];
    if (fread(magic, 1, NEBULA_MAGIC_LEN, f) != NEBULA_MAGIC_LEN) return -1;
    if (memcmp(magic, NEBULA_MAGIC, NEBULA_MAGIC_LEN) != 0) return -1;

    /* Skip hash_bytes (8 bytes) */
    if (fseek(f, 8, SEEK_CUR) != 0) return -1;

    /* obj_type (1 byte) */
    uint8_t obj_type_byte;
    if (fread(&obj_type_byte, 1, 1, f) != 1) return -1;

    /* author_len (1 byte) */
    uint8_t author_len;
    if (fread(&author_len, 1, 1, f) != 1) return -1;

    /* author (author_len bytes) */
    char author[64];
    if (author_len > 0) {
        size_t to_read = author_len < (uint8_t)sizeof(author) - 1
                         ? author_len
                         : (uint8_t)(sizeof(author) - 1);
        if (fread(author, 1, to_read, f) != to_read) return -1;
        author[to_read] = '\0';
        /* Skip any excess we couldn't fit (shouldn't happen; cap is 63) */
        if (to_read < author_len) {
            if (fseek(f, (long)(author_len - to_read), SEEK_CUR) != 0) return -1;
        }
    } else {
        author[0] = '\0';
    }

    /* Skip timestamp (8 bytes) */
    if (fseek(f, 8, SEEK_CUR) != 0) return -1;

    /* ndims (1 byte) */
    uint8_t ndims;
    if (fread(&ndims, 1, 1, f) != 1) return -1;
    if (ndims > STORAGE_MAX_NDIMS) return -1;

    /* shape: ndims * 4 bytes, big-endian uint32 each */
    uint32_t shape[STORAGE_MAX_NDIMS] = {0};
    for (int i = 0; i < ndims; i++) {
        uint8_t dim_bytes[4];
        if (fread(dim_bytes, 1, 4, f) != 4) return -1;
        shape[i] = be_to_u32(dim_bytes);
    }

    /* dtype (1 byte) */
    uint8_t dtype;
    if (fread(&dtype, 1, 1, f) != 1) return -1;

    /* content_len (4 bytes big-endian) */
    uint8_t cl_bytes[4];
    if (fread(cl_bytes, 1, 4, f) != 4) return -1;
    uint32_t clen = be_to_u32(cl_bytes);

    if (out) {
        out->obj_type       = (int)obj_type_byte;
        out->dtype          = (int)dtype;
        out->ndims          = (int)ndims;
        for (int i = 0; i < STORAGE_MAX_NDIMS; i++) out->shape[i] = shape[i];
        memcpy(out->author, author, sizeof(out->author));
        out->content_len    = (int)clen;
        out->content_offset = ftell(f);
    }
    return 0;
}

/* ── storage_get ──────────────────────────────────────────────────────────── */

int storage_get(const char *dir, const char *hash, char *buf, size_t buf_sz)
{
    char fpath[512];
    obj_path(dir, hash, fpath, sizeof(fpath));

    FILE *f = fopen(fpath, "rb");
    if (!f) return -1;

    StorageHeader h;
    if (read_header(f, &h) != 0) {
        fclose(f);
        return -1;
    }

    int clen = h.content_len;
    if ((size_t)clen >= buf_sz) {
        /* truncate to fit, leave room for NUL */
        clen = (int)(buf_sz - 1);
    }

    size_t n = fread(buf, 1, (size_t)clen, f);
    fclose(f);

    buf[n] = '\0';
    return (int)n;
}

/* ── storage_header ───────────────────────────────────────────────────────── */

int storage_header(const char *dir, const char *hash, StorageHeader *out)
{
    if (!out) return -1;

    char fpath[512];
    obj_path(dir, hash, fpath, sizeof(fpath));

    FILE *f = fopen(fpath, "rb");
    if (!f) return -1;

    int rc = read_header(f, out);
    fclose(f);
    return rc;
}

/* ── storage_path ─────────────────────────────────────────────────────────── */

int storage_path(const char *dir, const char *hash,
                 char *path_out, size_t path_sz)
{
    char fpath[512];
    obj_path(dir, hash, fpath, sizeof(fpath));

    FILE *f = fopen(fpath, "rb");
    if (!f) return -1;
    fclose(f);

    snprintf(path_out, path_sz, "%s", fpath);
    return 0;
}

/* ── storage_exists ───────────────────────────────────────────────────────── */

int storage_exists(const char *dir, const char *hash)
{
    char fpath[512];
    obj_path(dir, hash, fpath, sizeof(fpath));

    FILE *f = fopen(fpath, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ── storage_content_len ─────────────────────────────────────────────────── */

int storage_content_len(const char *dir, const char *hash)
{
    StorageHeader h;
    if (storage_header(dir, hash, &h) != 0) return -1;
    return h.content_len;
}

/* ── storage_content_offset ──────────────────────────────────────────────── */

long storage_content_offset(const char *dir, const char *hash)
{
    StorageHeader h;
    if (storage_header(dir, hash, &h) != 0) return -1;
    return h.content_offset;
}
