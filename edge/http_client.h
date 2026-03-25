/*
 * http_client.h — Minimal blocking HTTP/1.0 GET client.
 *
 * Used by worker agents to fetch data objects from sentient peers over TCP.
 * Accepts only IPv4 address strings (no DNS resolution).
 */

#ifndef EDGE_HTTP_CLIENT_H
#define EDGE_HTTP_CLIENT_H

#include <stdint.h>
#include <stddef.h>

/*
 * Perform a blocking HTTP/1.0 GET request to host:port/path.
 *
 * The full response (headers + body) is read into out; then the body is
 * moved to the front of out, NUL-terminated, and its length returned.
 * out_sz must be large enough for both the response headers (~2–4 KB)
 * and the response body; if the server sends more than out_sz-1 bytes,
 * reading is truncated but the body is still returned.
 *
 * host   — IPv4 address string (e.g. "192.168.1.5")
 * port   — TCP port
 * path   — request path including query string (e.g. "/data/get?name=foo")
 * out    — caller-supplied buffer; receives body as NUL-terminated string
 * out_sz — size of out in bytes
 *
 * Returns body length (>= 0) on success, -1 on connection/protocol error.
 */
int http_get(const char *host, uint16_t port, const char *path,
             char *out, size_t out_sz);

#endif /* EDGE_HTTP_CLIENT_H */
