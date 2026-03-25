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
 */

#ifndef EDGE_STORAGE_H
#define EDGE_STORAGE_H

#include <stddef.h>

#define STORAGE_OBJ_PROGRAM 1
#define STORAGE_OBJ_DATA    2

/*
 * Store content as a NebulaDisk object.
 * Creates directory {dir}/objects/{hash[0:2]}/ if needed.
 * If the object already exists (same hash), skips writing and returns 0.
 *
 * hash_out: 17-byte buffer to receive the 16-char hex hash + NUL.
 * Returns 0 on success, -1 on error.
 */
int storage_put(const char *dir, const char *content, size_t len,
                int obj_type, const char *author, const char *name,
                char *hash_out);

/*
 * Retrieve content by hash. Parses the NebulaDisk header and copies only
 * the content section into buf (NUL-terminated).
 * Returns content length (>= 0) on success, -1 if not found or error.
 */
int storage_get(const char *dir, const char *hash, char *buf, size_t buf_sz);

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

#endif /* EDGE_STORAGE_H */
