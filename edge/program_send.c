/*
 * program_send.c — Compact-encode an NML program and broadcast via UDP.
 */

#include "program_send.h"
#include "msg.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Pull in nml_sign_program as a static function. */
#define NML_CRYPTO
#include "../../nml/runtime/nml_crypto.h"

int program_send(UDPContext *udp,
                 const char *agent_name, uint16_t agent_port,
                 const char *program_text,
                 const char *key_hex, const char *key_agent)
{
    /* Optionally sign first */
    static char signed_buf[NML_MAX_PROGRAM_LEN + 512]; /* extra room for SIGN header */
    const char *source = program_text;

    if (key_hex && key_agent) {
        int rc = nml_sign_program(program_text, key_agent,
                                  key_hex, signed_buf, sizeof(signed_buf));
        if (rc < 0) {
            fprintf(stderr, "[program_send] signing failed\n");
            return -1;
        }
        source = signed_buf;
    }

    /* Compact-encode */
    static char compact[NML_MAX_PROGRAM_LEN + 1];
    if (msg_program_to_compact(source, compact, sizeof(compact)) < 0) {
        fprintf(stderr, "[program_send] compact encode overflow\n");
        return -1;
    }

    /* Wire-encode as MSG_PROGRAM */
    static uint8_t wire[NML_MAX_PROGRAM_LEN + 256];
    int n = msg_encode(wire, sizeof(wire),
                       MSG_PROGRAM, agent_name, agent_port, compact);
    if (n < 0) {
        fprintf(stderr, "[program_send] wire encode overflow\n");
        return -1;
    }

    return udp_send(udp, UDP_MULTICAST_GROUP, UDP_MULTICAST_PORT,
                    wire, (size_t)n);
}
