/*
 * http_client.c — Minimal blocking HTTP/1.0 GET client.
 */

#include "http_client.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int http_get(const char *host, uint16_t port, const char *path,
             char *out, size_t out_sz)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Send request — HTTP/1.0 so the server closes the connection after the
       response body, which tells us when reading is done. */
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
        path, host, (unsigned)port);
    if (req_len <= 0 || (size_t)req_len >= sizeof(req)) {
        close(fd);
        return -1;
    }
    if (send(fd, req, (size_t)req_len, 0) != req_len) {
        close(fd);
        return -1;
    }

    /* Read until the server closes the connection (HTTP/1.0 EOF-terminated).
       Leave 1 byte at end for NUL. */
    int total = 0;
    int n;
    while ((size_t)total < out_sz - 1) {
        n = (int)recv(fd, out + total, out_sz - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    if (total <= 0) return -1;
    out[total] = '\0';

    /* Locate the blank line separating headers from body (\r\n\r\n). */
    char *body = NULL;
    for (int i = 0; i <= total - 4; i++) {
        if (out[i] == '\r' && out[i+1] == '\n' &&
            out[i+2] == '\r' && out[i+3] == '\n') {
            body = out + i + 4;
            break;
        }
    }
    if (!body) return -1;

    int body_len = total - (int)(body - out);
    if (body_len < 0) return -1;

    /* Slide body to the front of the buffer. */
    memmove(out, body, (size_t)body_len);
    out[body_len] = '\0';
    return body_len;
}
