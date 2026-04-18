/*
 * http_util.h — Shared HTTP server primitives for NML role agents.
 *
 * Every role agent runs its own small HTTP server. These helpers factor out
 * the pieces that were being copy-pasted into each agent:
 *   - socket bind / listen
 *   - JSON / HTML / binary response writing with CORS headers
 *   - Content-Length–aware request read
 *   - minimal JSON string-value extraction
 */

#ifndef EDGE_HTTP_UTIL_H
#define EDGE_HTTP_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include "compat.h"

/*
 * Bind a TCP server socket to INADDR_ANY:port with SO_REUSEADDR and listen.
 * Returns the listening socket, or COMPAT_INVALID_SOCKET on failure.
 */
compat_socket_t http_listen(uint16_t port);

/*
 * Send a JSON response body with Access-Control-Allow-Origin: * and
 * Connection: close. Handles the most common status codes used across roles
 * (200, 201, 400, 401, 403, 404, 405, 413, 429, 500, 503).
 */
void http_send_json(compat_socket_t fd, int status, const char *body);

/*
 * Send a text/html response with Cache-Control: no-cache.
 * `body` need not be NUL-terminated; `len` bytes are written verbatim.
 */
void http_send_html(compat_socket_t fd, const char *body, size_t len);

/*
 * Send a binary response with a caller-supplied content type.
 */
void http_send_binary(compat_socket_t fd, const char *content_type,
                      const char *data, size_t len);

/*
 * Receive an HTTP request into buf, looping recv until the full body
 * described by Content-Length has arrived.
 *
 * Returns:
 *    > 0   — total bytes received (including headers + body)
 *    -1    — I/O error or premature EOF
 *    -2    — Content-Length > max_body_sz
 */
int http_recv_full(compat_socket_t fd, char *buf, int buf_sz, int max_body_sz);

/*
 * Extract a string value for `"key": "<value>"` from `json` into `out`.
 * Handles one level of backslash escape (`\"`, `\\`). Returns the number
 * of bytes written (excluding NUL), or -1 if the key is absent or not a
 * quoted string.
 */
int http_json_str(const char *json, const char *key,
                  char *out, size_t out_sz);

#endif /* EDGE_HTTP_UTIL_H */
