#include "udp.h"
#include "compat.h"

#include <string.h>
#include <stdio.h>
#include <limits.h>

int udp_init(UDPContext *ctx, const char *mcast_group, uint16_t port)
{
    ctx->send_fd = COMPAT_INVALID_SOCKET;
    ctx->recv_fd = COMPAT_INVALID_SOCKET;

    /* ── Send socket ── */
    ctx->send_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->send_fd == COMPAT_INVALID_SOCKET) {
        perror("[udp] socket(send)");
        return -1;
    }
    int ttl = 2;
    setsockopt(ctx->send_fd, IPPROTO_IP, IP_MULTICAST_TTL,
               COMPAT_SOCKOPT_CAST(&ttl), sizeof(ttl));
    int loop = 1;
    setsockopt(ctx->send_fd, IPPROTO_IP, IP_MULTICAST_LOOP,
               COMPAT_SOCKOPT_CAST(&loop), sizeof(loop));

    /* ── Receive socket ── */
    ctx->recv_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->recv_fd == COMPAT_INVALID_SOCKET) {
        perror("[udp] socket(recv)");
        compat_close_socket(ctx->send_fd); ctx->send_fd = COMPAT_INVALID_SOCKET;
        return -1;
    }
    int reuse = 1;
    setsockopt(ctx->recv_fd, SOL_SOCKET, SO_REUSEADDR,
               COMPAT_SOCKOPT_CAST(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(ctx->recv_fd, SOL_SOCKET, SO_REUSEPORT,
               COMPAT_SOCKOPT_CAST(&reuse), sizeof(reuse));
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ctx->recv_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("[udp] bind");
        compat_close_socket(ctx->send_fd); compat_close_socket(ctx->recv_fd);
        ctx->send_fd = ctx->recv_fd = COMPAT_INVALID_SOCKET;
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_group);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(ctx->recv_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   COMPAT_SOCKOPT_CAST(&mreq), sizeof(mreq)) < 0) {
        fprintf(stderr, "[udp] failed to join %s\n", mcast_group);
        compat_close_socket(ctx->send_fd); compat_close_socket(ctx->recv_fd);
        ctx->send_fd = ctx->recv_fd = COMPAT_INVALID_SOCKET;
        return -1;
    }

    return 0;
}

void udp_close(UDPContext *ctx)
{
    if (ctx->send_fd != COMPAT_INVALID_SOCKET) {
        compat_close_socket(ctx->send_fd);
        ctx->send_fd = COMPAT_INVALID_SOCKET;
    }
    if (ctx->recv_fd != COMPAT_INVALID_SOCKET) {
        compat_close_socket(ctx->recv_fd);
        ctx->recv_fd = COMPAT_INVALID_SOCKET;
    }
}

int udp_send(UDPContext *ctx, const char *mcast_group, uint16_t port,
             const uint8_t *buf, size_t len)
{
    /* Winsock's sendto takes int; reject rather than silently truncate.
       (UDP's own payload cap is ~65507, so anything over INT_MAX is already
       invalid at the protocol level.) */
    if (len > (size_t)INT_MAX) return -1;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = inet_addr(mcast_group);
    return (int)sendto(ctx->send_fd, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
}

int udp_recv(UDPContext *ctx, uint8_t *buf, size_t buf_sz, int timeout_ms,
             char *sender_ip_out)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ctx->recv_fd, &fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000L;

    struct timeval *tvp = timeout_ms < 0 ? NULL : &tv;
    int rc = select(COMPAT_SELECT_NFDS(ctx->recv_fd), &fds, NULL, NULL, tvp);
    if (rc <= 0) return rc;

    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    /* Clamp buf_sz to INT_MAX for Winsock's int-typed length; this caps how
       much the kernel may write, so it's safe wrt the caller's buffer. */
    int recv_cap = (buf_sz > (size_t)INT_MAX) ? INT_MAX : (int)buf_sz;
    int n = (int)recvfrom(ctx->recv_fd, (char *)buf, recv_cap, 0,
                          (struct sockaddr *)&src, &src_len);
    if (n > 0 && sender_ip_out) {
        inet_ntop(AF_INET, &src.sin_addr, sender_ip_out, 46);
    }
    return n;
}
