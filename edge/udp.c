#include "udp.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int udp_init(UDPContext *ctx, const char *mcast_group, uint16_t port)
{
    ctx->send_fd = -1;
    ctx->recv_fd = -1;

    /* ── Send socket ── */
    ctx->send_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->send_fd < 0) {
        perror("[udp] socket(send)");
        return -1;
    }
    int ttl = 2;
    setsockopt(ctx->send_fd, IPPROTO_IP, IP_MULTICAST_TTL,  &ttl, sizeof(ttl));
    int loop = 1;
    setsockopt(ctx->send_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    /* ── Receive socket ── */
    ctx->recv_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->recv_fd < 0) {
        perror("[udp] socket(recv)");
        close(ctx->send_fd); ctx->send_fd = -1;
        return -1;
    }
    int reuse = 1;
    setsockopt(ctx->recv_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(ctx->recv_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ctx->recv_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("[udp] bind");
        close(ctx->send_fd); close(ctx->recv_fd);
        ctx->send_fd = ctx->recv_fd = -1;
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_group);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(ctx->recv_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "[udp] failed to join %s\n", mcast_group);
        close(ctx->send_fd); close(ctx->recv_fd);
        ctx->send_fd = ctx->recv_fd = -1;
        return -1;
    }

    return 0;
}

void udp_close(UDPContext *ctx)
{
    if (ctx->send_fd >= 0) { close(ctx->send_fd); ctx->send_fd = -1; }
    if (ctx->recv_fd >= 0) { close(ctx->recv_fd); ctx->recv_fd = -1; }
}

int udp_send(UDPContext *ctx, const char *mcast_group, uint16_t port,
             const uint8_t *buf, size_t len)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = inet_addr(mcast_group);
    return (int)sendto(ctx->send_fd, buf, len, 0,
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
    int rc = select(ctx->recv_fd + 1, &fds, NULL, NULL, tvp);
    if (rc <= 0) return rc;

    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    int n = (int)recvfrom(ctx->recv_fd, buf, buf_sz, 0,
                          (struct sockaddr *)&src, &src_len);
    if (n > 0 && sender_ip_out) {
        inet_ntop(AF_INET, &src.sin_addr, sender_ip_out, 46);
    }
    return n;
}
