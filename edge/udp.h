/*
 * NML Edge Worker — UDP multicast send/receive.
 *
 * Two sockets:
 *   send_fd — SOCK_DGRAM with IP_MULTICAST_TTL=2, IP_MULTICAST_LOOP=1
 *   recv_fd — bound to 0.0.0.0:<port>, joined to multicast group
 */

#ifndef EDGE_UDP_H
#define EDGE_UDP_H

#include <stdint.h>
#include <stddef.h>
#include "compat.h"

typedef struct {
    compat_socket_t send_fd;
    compat_socket_t recv_fd;
} UDPContext;

/* Initialise both sockets and join the multicast group.
   Returns 0 on success, -1 on error. */
int udp_init(UDPContext *ctx, const char *mcast_group, uint16_t port);

void udp_close(UDPContext *ctx);

/* Send buf to mcast_group:port. Returns bytes sent or -1. */
int udp_send(UDPContext *ctx, const char *mcast_group, uint16_t port,
             const uint8_t *buf, size_t len);

/* Receive with timeout_ms (-1 = block forever).
   Writes sender IPv4 address string into sender_ip_out if non-NULL (must be >= 46 bytes).
   Returns bytes received, 0 on timeout, -1 on error. */
int udp_recv(UDPContext *ctx, uint8_t *buf, size_t buf_sz, int timeout_ms,
             char *sender_ip_out);

#endif /* EDGE_UDP_H */
