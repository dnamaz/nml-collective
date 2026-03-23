#include "report.h"
#include "msg.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

int report_send_udp(UDPContext *ctx,
                    const char *agent_name, uint16_t agent_port,
                    const char *phash, const char *score)
{
    /* Payload: "{phash}:{score}" — matches Python's f"{phash}:{score:.6f}" */
    char payload[64];
    int n = snprintf(payload, sizeof(payload), "%s:%s", phash, score);
    if (n < 0 || (size_t)n >= sizeof(payload)) return -1;

    uint8_t buf[256];
    int len = msg_encode(buf, sizeof(buf),
                         MSG_RESULT, agent_name, agent_port, payload);
    if (len < 0) return -1;

    int sent = udp_send(ctx, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT, buf, (size_t)len);
    return sent > 0 ? 0 : -1;
}

int report_send_http(const char *url, const char *phash, const char *score)
{
#if USE_HTTP_REPORT
    /* Minimal HTTP POST using a platform TCP socket.
       For Linux with libcurl available, link with -lcurl and implement here.
       For bare-metal, use Mongoose or lwIP. */
    (void)url; (void)phash; (void)score;
    fprintf(stderr, "[report] HTTP reporting not yet implemented\n");
    return -1;
#else
    (void)url; (void)phash; (void)score;
    return -1;
#endif
}
