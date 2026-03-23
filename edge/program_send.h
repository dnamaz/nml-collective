/*
 * program_send.h — Compact-encode an NML program and broadcast via UDP.
 *
 * Wraps msg_program_to_compact() + msg_encode(MSG_PROGRAM) + udp_send().
 * Optionally prepends a SIGN header via crypto_sign_program() if a key
 * is provided (pass NULL key_hex to send unsigned).
 */

#ifndef COLLECTIVE_PROGRAM_SEND_H
#define COLLECTIVE_PROGRAM_SEND_H

#include "udp.h"

/*
 * Compact-encode program_text and broadcast it as MSG_PROGRAM.
 *
 * agent_name  — sender name embedded in the wire header
 * agent_port  — sender port embedded in the wire header
 * program_text — multi-line NML source (comment lines starting ';' stripped)
 * key_hex     — NULL → unsigned; otherwise "<hmac-sha256|ed25519>:<hex>"
 *               (same format accepted by crypto_verify_program)
 * key_agent   — agent name embedded in the SIGN header (only used if key_hex != NULL)
 *
 * Returns bytes sent, or -1 on error (encode overflow, UDP failure).
 */
int program_send(UDPContext *udp,
                 const char *agent_name, uint16_t agent_port,
                 const char *program_text,
                 const char *key_hex, const char *key_agent);

#endif /* COLLECTIVE_PROGRAM_SEND_H */
