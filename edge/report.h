/*
 * NML Edge Worker — result reporting.
 *
 * MSG_RESULT payload format (matches nml_collective.py:broadcast_result()):
 *   "{program_hash16}:{score:.6f}"
 */

#ifndef EDGE_REPORT_H
#define EDGE_REPORT_H

#include "udp.h"

/*
 * Send a MSG_RESULT via UDP multicast.
 * phash is the 16-char hex program hash.
 * score is the decimal score string (e.g. "0.823141").
 * Returns 0 on success, -1 on error.
 */
int report_send_udp(UDPContext *ctx,
                    const char *agent_name, uint16_t agent_port,
                    const char *phash, const char *score);

/*
 * HTTP POST result to a sentient (requires USE_HTTP_REPORT=1 and libcurl or
 * a minimal HTTP client). Stub for now — define USE_HTTP_REPORT=1 to enable.
 * Returns 0 on success, -1 on error or if not compiled in.
 */
int report_send_http(const char *url,
                     const char *phash, const char *score);

#endif /* EDGE_REPORT_H */
