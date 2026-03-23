/*
 * udp.c — lwIP UDP stub for NML Edge Worker on ARM Cortex-M4.
 *
 * This file replaces the POSIX udp.c when building for bare metal.
 * It compiles cleanly and provides the same udp.h API surface; the
 * actual lwIP / Ethernet driver integration is left as TODO hooks so
 * you can drop it into your BSP without merge conflicts.
 *
 * Integration checklist:
 *   1. Include lwip/udp.h and call lwip_init() / ethernetif_init() in your BSP.
 *   2. Implement arm_edge_udp_recv_hook() to receive packets from your
 *      Ethernet ISR / lwIP callback and write them into _rx_buf.
 *   3. Implement arm_edge_udp_send_hook() to call udp_sendto() via lwIP.
 *   4. Call arm_edge_tick() from your main loop or FreeRTOS task.
 *
 * All function signatures match the POSIX udp.h exactly.
 */

#include "../../udp.h"   /* shared API header from edge/ */

#include <string.h>
#include <stdint.h>

/* ── RX ring buffer ──────────────────────────────────────────────────────── */

#define RX_BUF_SZ  2048u
#define RX_MAX_PKT   16u

typedef struct {
    uint8_t  data[RX_BUF_SZ];
    uint16_t len;
} RxPacket;

static RxPacket  _rx_ring[RX_MAX_PKT];
static volatile uint32_t _rx_head = 0;
static volatile uint32_t _rx_tail = 0;

/* Called from ISR / lwIP receive callback — enqueue one packet */
void arm_edge_udp_enqueue(const uint8_t *data, uint16_t len)
{
    uint32_t next = (_rx_head + 1u) % RX_MAX_PKT;
    if (next == _rx_tail) return;   /* ring full — drop */

    uint16_t n = len < RX_BUF_SZ ? len : (uint16_t)RX_BUF_SZ;
    memcpy(_rx_ring[_rx_head].data, data, n);
    _rx_ring[_rx_head].len = n;
    _rx_head = next;
}

/* ── BSP hooks (weak — override in your BSP) ─────────────────────────────── */

/*
 * Send a UDP datagram to mcast_group:port.
 *
 * Override this with your lwIP call:
 *   void arm_edge_udp_send_hook(const char *grp, uint16_t port,
 *                               const uint8_t *buf, uint16_t len) {
 *       ip_addr_t ip;
 *       ipaddr_aton(grp, &ip);
 *       udp_sendto(_upcb, pbuf_alloc_reference((void*)buf,len,PBUF_REF), &ip, port);
 *   }
 */
__attribute__((weak))
void arm_edge_udp_send_hook(const char *mcast_group, uint16_t port,
                             const uint8_t *buf, uint16_t len)
{
    (void)mcast_group; (void)port; (void)buf; (void)len;
    /* TODO: implement with lwIP udp_sendto() */
}

/* ── udp.h API implementation ────────────────────────────────────────────── */

int udp_init(UDPContext *ctx, const char *mcast_group, uint16_t port)
{
    (void)mcast_group; (void)port;
    ctx->send_fd = 0;   /* not used; field kept for API compatibility */
    ctx->recv_fd = 0;
    /* TODO: call lwip_init(), create udp_pcb, bind to port, join multicast group */
    return 0;
}

void udp_close(UDPContext *ctx)
{
    (void)ctx;
    /* TODO: udp_remove(upcb) */
}

int udp_send(UDPContext *ctx, const char *mcast_group, uint16_t port,
             const uint8_t *buf, size_t len)
{
    (void)ctx;
    arm_edge_udp_send_hook(mcast_group, port, buf, (uint16_t)len);
    return (int)len;
}

/*
 * Blocking receive with timeout_ms milliseconds.
 * On bare metal, "blocking" means polling the RX ring until a packet
 * arrives or the tick counter advances by timeout_ms.
 */
int udp_recv(UDPContext *ctx, uint8_t *buf, size_t buf_sz, int timeout_ms)
{
    (void)ctx;

    /* TODO: replace with a real tick source */
    /* For now we poll up to timeout_ms iterations */
    int iters = timeout_ms > 0 ? timeout_ms : 1;
    for (int i = 0; i < iters; i++) {
        if (_rx_head != _rx_tail) {
            RxPacket *pkt = &_rx_ring[_rx_tail];
            size_t    n   = pkt->len < buf_sz ? pkt->len : buf_sz;
            memcpy(buf, pkt->data, n);
            _rx_tail = (_rx_tail + 1u) % RX_MAX_PKT;
            return (int)n;
        }
        /* TODO: yield to RTOS scheduler or call sys_check_timeouts() here */
    }
    return 0;   /* timeout */
}
