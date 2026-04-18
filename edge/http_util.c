/*
 * http_util.c — shared HTTP server primitives.
 *
 * See http_util.h for API docs. Behavior matches the inlined copies that
 * previously lived in each role agent, so migration is a mechanical
 * rename / include-swap.
 */

#define _POSIX_C_SOURCE 200809L

#include "http_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── http_listen ────────────────────────────────────────────────────── */

compat_socket_t http_listen(uint16_t port)
{
    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COMPAT_INVALID_SOCKET) return COMPAT_INVALID_SOCKET;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               COMPAT_SOCKOPT_CAST(&one), sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        compat_close_socket(fd);
        return COMPAT_INVALID_SOCKET;
    }
    return fd;
}

/* ── Status text ────────────────────────────────────────────────────── */

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Error";
    }
}

/* ── http_send_json ─────────────────────────────────────────────────── */

void http_send_json(compat_socket_t fd, int status, const char *body)
{
    size_t body_len = body ? strlen(body) : 0;

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n\r\n",
        status, status_text(status), body_len);

    send(fd, hdr, (size_t)hdr_len, 0);
    if (body && body_len > 0) {
        send(fd, body, body_len, 0);
    }
}

/* ── http_send_html ─────────────────────────────────────────────────── */

void http_send_html(compat_socket_t fd, const char *body, size_t len)
{
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        len);
    send(fd, hdr, (size_t)hdr_len, 0);
    send(fd, body, len, 0);
}

/* ── http_send_binary ──────────────────────────────────────────────── */

void http_send_binary(compat_socket_t fd, const char *content_type,
                      const char *data, size_t len)
{
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        content_type, len);
    send(fd, hdr, (size_t)hdr_len, 0);
    send(fd, data, len, 0);
}

/* ── http_recv_full ─────────────────────────────────────────────────── */

int http_recv_full(compat_socket_t fd, char *buf, int buf_sz, int max_body_sz)
{
    int n = recv(fd, buf, buf_sz - 1, 0);
    if (n <= 0) return -1;
    buf[n] = '\0';

    const char *cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    if (!cl) return n;

    int content_length = atoi(cl + 15);
    if (content_length <= 0) return n;
    if (content_length > max_body_sz) return -2;

    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) return n;

    int hdr_sz        = (int)(hdr_end + 4 - buf);
    int body_received = n - hdr_sz;
    int body_needed   = content_length - body_received;

    while (body_needed > 0 && (n + body_needed) < buf_sz) {
        int chunk = recv(fd, buf + n, (size_t)body_needed, 0);
        if (chunk <= 0) break;
        n += chunk;
        body_needed -= chunk;
    }
    buf[n] = '\0';
    return n;
}

/* ── http_json_str ─────────────────────────────────────────────────── */

int http_json_str(const char *json, const char *key,
                  char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0) return -1;

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);

    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (int)i;
}
