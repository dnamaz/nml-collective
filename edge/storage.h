/*
 * storage.h — Minimal content-addressed NebulaDisk object store.
 *
 * Objects are stored as binary files matching the NebulaDisk format
 * (nml_storage.py).  Each file lives at:
 *   {dir}/objects/{hash[0:2]}/{hash}.obj
 *
 * Object types:
 *   STORAGE_OBJ_PROGRAM = 1
 *   STORAGE_OBJ_DATA    = 2
 *
 * Dtype codes (the 1-byte dtype field inside the header):
 *   STORAGE_DTYPE_NONE    = 0   unknown / unset
 *   STORAGE_DTYPE_UINT8   = 1
 *   STORAGE_DTYPE_FLOAT32 = 2
 *   STORAGE_DTYPE_FLOAT64 = 3
 *   STORAGE_DTYPE_TEXT    = 4   opaque bytes / JSON / NML text
 */

#ifndef EDGE_STORAGE_H
#define EDGE_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#define STORAGE_OBJ_PROGRAM 1
#define STORAGE_OBJ_DATA    2

#define STORAGE_DTYPE_NONE    0
#define STORAGE_DTYPE_UINT8   1
#define STORAGE_DTYPE_FLOAT32 2
#define STORAGE_DTYPE_FLOAT64 3
#define STORAGE_DTYPE_TEXT    4

#define STORAGE_MAX_NDIMS 8

/*
 * Parsed NebulaDisk header. Populated by storage_header().
 */
typedef struct {
    int      obj_type;                        /* STORAGE_OBJ_*              */
    int      dtype;                           /* STORAGE_DTYPE_*            */
    int      ndims;                           /* 0..STORAGE_MAX_NDIMS       */
    uint32_t shape[STORAGE_MAX_NDIMS];        /* dim sizes, big-endian u32  */
    char     author[64];                      /* NUL-terminated             */
    int      content_len;                     /* payload byte count         */
    long     content_offset;                  /* byte offset to payload     */
} StorageHeader;

/*
 * Store content as a NebulaDisk object.
 * Creates directory {dir}/objects/{hash[0:2]}/ if needed.
 * If the object already exists (same hash), skips writing and returns 0.
 *
 * This wrapper writes dtype=TEXT, ndims=0 for backward compatibility with
 * callers that treat content as opaque bytes. Prefer storage_put_shaped()
 * when you know the dtype and shape.
 *
 * hash_out: 17-byte buffer to receive the 16-char hex hash + NUL.
 * Returns 0 on success, -1 on error.
 */
int storage_put(const char *dir, const char *content, size_t len,
                int obj_type, const char *author, const char *name,
                char *hash_out);

/*
 * Store content with a real dtype and shape in the header.
 * ndims must be in [0, STORAGE_MAX_NDIMS]; shape may be NULL iff ndims == 0.
 * Returns 0 on success, -1 on error.
 */
int storage_put_shaped(const char *dir, const char *content, size_t len,
                       int obj_type, int dtype,
                       int ndims, const uint32_t *shape,
                       const char *author, const char *name,
                       char *hash_out);

/*
 * Retrieve content by hash. Parses the NebulaDisk header and copies only
 * the content section into buf (NUL-terminated).
 * Returns content length (>= 0) on success, -1 if not found or error.
 */
int storage_get(const char *dir, const char *hash, char *buf, size_t buf_sz);

/*
 * Parse the NebulaDisk header for the given hash into *out.
 * Returns 0 on success, -1 if not found or header is malformed.
 */
int storage_header(const char *dir, const char *hash, StorageHeader *out);

/*
 * Get the on-disk path for an object. Writes into path_out (size >= 300).
 * Returns 0 on success, -1 if not found.
 */
int storage_path(const char *dir, const char *hash,
                 char *path_out, size_t path_sz);

/*
 * Check if an object exists. Returns 1 if yes, 0 if no.
 */
int storage_exists(const char *dir, const char *hash);

/*
 * Return the content length of a stored object without reading content.
 * Returns length >= 0 on success, -1 on error/not found.
 */
int storage_content_len(const char *dir, const char *hash);

/*
 * Return the byte offset from the start of the .obj file to the first byte
 * of the content payload (immediately after the NebulaDisk header).
 * Used by range-serving to seek directly to a float sub-range.
 * Returns >= 0 on success, -1 on error/not found.
 */
long storage_content_offset(const char *dir, const char *hash);

#endif /* EDGE_STORAGE_H */
