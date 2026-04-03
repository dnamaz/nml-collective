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
#define DTYPE_TEXT 4

/* ── Internal path helper ─────────────────────────────────────────────────── */

static void obj_path(const char *dir, const char *hash, char *out, size_t sz)
{
    /* {dir}/objects/{hash[0:2]}/{hash}.obj */
    snprintf(out, sz, "%s/objects/%.2s/%s.obj", dir, hash, hash);
}

/* ── storage_put ──────────────────────────────────────────────────────────── */

int storage_put(const char *dir, const char *content, size_t len,
                int obj_type, const char *author, const char *name,
                char *hash_out)
{
    (void)name; /* name is metadata only; hash is the canonical key */

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

    /* ndims: 0 as 1 byte */
    {
        uint8_t nd = 0;
        fwrite(&nd, 1, 1, f);
    }

    /* dtype: 4 (DTYPE_TEXT) as 1 byte */
    {
        uint8_t dt = DTYPE_TEXT;
        fwrite(&dt, 1, 1, f);
    }

    /* content_len: (uint32_t)len written big-endian (4 bytes) */
    {
        uint32_t clen = (uint32_t)len;
        uint8_t cl_bytes[4];
        cl_bytes[0] = (uint8_t)((clen >> 24) & 0xFF);
        cl_bytes[1] = (uint8_t)((clen >> 16) & 0xFF);
        cl_bytes[2] = (uint8_t)((clen >>  8) & 0xFF);
        cl_bytes[3] = (uint8_t)((clen      ) & 0xFF);
        fwrite(cl_bytes, 1, 4, f);
    }

    /* content: len bytes */
    fwrite(content, 1, len, f);

    fclose(f);
    return 0;
}

/* ── Internal: skip to content_len field and return it ───────────────────── */

static int read_content_len(FILE *f)
{
    uint8_t magic[NEBULA_MAGIC_LEN];
    if (fread(magic, 1, NEBULA_MAGIC_LEN, f) != NEBULA_MAGIC_LEN) return -1;
    if (memcmp(magic, NEBULA_MAGIC, NEBULA_MAGIC_LEN) != 0) return -1;

    /* Skip hash_bytes (8 bytes) */
    if (fseek(f, 8, SEEK_CUR) != 0) return -1;

    /* Read obj_type (1 byte, discard) */
    uint8_t obj_type_byte;
    if (fread(&obj_type_byte, 1, 1, f) != 1) return -1;

    /* Read author_len (1 byte) */
    uint8_t author_len;
    if (fread(&author_len, 1, 1, f) != 1) return -1;

    /* Skip author + timestamp (author_len + 8 bytes) */
    if (fseek(f, (long)(author_len + 8), SEEK_CUR) != 0) return -1;

    /* Read ndims (1 byte) */
    uint8_t ndims;
    if (fread(&ndims, 1, 1, f) != 1) return -1;

    /* Skip shape: ndims * 4 bytes */
    if (ndims > 0) {
        if (fseek(f, (long)(ndims * 4), SEEK_CUR) != 0) return -1;
    }

    /* Skip dtype (1 byte) */
    uint8_t dtype;
    if (fread(&dtype, 1, 1, f) != 1) return -1;

    /* Read content_len: 4 bytes big-endian */
    uint8_t cl_bytes[4];
    if (fread(cl_bytes, 1, 4, f) != 4) return -1;

    uint32_t clen = ((uint32_t)cl_bytes[0] << 24) |
                    ((uint32_t)cl_bytes[1] << 16) |
                    ((uint32_t)cl_bytes[2] <<  8) |
                    ((uint32_t)cl_bytes[3]      );
    return (int)clen;
}

/* ── storage_get ──────────────────────────────────────────────────────────── */

int storage_get(const char *dir, const char *hash, char *buf, size_t buf_sz)
{
    char fpath[512];
    obj_path(dir, hash, fpath, sizeof(fpath));

    FILE *f = fopen(fpath, "rb");
    if (!f) return -1;

    int clen = read_content_len(f);
    if (clen < 0) {
        fclose(f);
        return -1;
    }

    if ((size_t)clen >= buf_sz) {
        /* truncate to fit, leave room for NUL */
        clen = (int)(buf_sz - 1);
    }

    size_t n = fread(buf, 1, (size_t)clen, f);
    fclose(f);

    buf[n] = '\0';
    return (int)n;
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
    char fpath[512];
    obj_path(dir, hash, fpath, sizeof(fpath));

    FILE *f = fopen(fpath, "rb");
    if (!f) return -1;

    int clen = read_content_len(f);
    fclose(f);
    return clen;
}
