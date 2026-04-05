/*
 * NML Edge Worker — wire format encode/decode and compact format conversion.
 *
 * Wire format (from nml_collective.py):
 *   4 bytes: magic "NML\x01"
 *   1 byte:  msg_type
 *   1 byte:  name_len  (max 63; capped on encode)
 *   N bytes: agent_name (UTF-8)
 *   2 bytes: http_port (big-endian)
 *   rest:    payload (UTF-8)
 *
 * MSG_PROGRAM payload uses pilcrow (0xB6) as a line delimiter:
 *   compact = join(0xB6, [stripped_line for line in program if not comment])
 *
 * MSG_RESULT payload format: "{hash16}:{score:.6f}"
 */

#ifndef EDGE_MSG_H
#define EDGE_MSG_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

/* Message types — must match nml_collective.py */
#define MSG_ANNOUNCE  1
#define MSG_PROGRAM   2
#define MSG_RESULT    3
#define MSG_HEARTBEAT 4
/* C-collective extension: enforcement gossip (type 5 ignored by Python agents) */
#define MSG_ENFORCE   5
/* Oracle → Architect program specification (type 6 ignored by Python agents) */
#define MSG_SPEC      6
/*
 * Governance vote from any collective node (type 7).
 * Payload: "{hash16}:{score:.6f}"  — same format as MSG_RESULT.
 * Voter identity is carried in the wire-format name field.
 * Oracle feeds these into the same weighted z-score pipeline as MSG_RESULT,
 * enabling full soft-voting consensus across all roles, not just workers.
 *
 * Vote semantics per role:
 *   sentient  — 1.0 signed / 0.8 unsigned (no key configured)
 *   enforcer  — 1.0 no outlier / 0.0 outlier detected for this phash
 *   architect — 1.0 template / 0.9 llm+validated / 0.6 llm+unvalidated
 */
#define MSG_VOTE      7

/* Minimum valid packet: magic(4)+type(1)+name_len(1)+port(2) */
#define MSG_HEADER_MIN 8

/*
 * Parse a raw UDP packet into its fields.
 * name_sz must be >= 64 (names are capped at 63 bytes + NUL on encode).
 * payload_sz should be NML_MAX_PROGRAM_LEN+1 or larger.
 * Returns 0 on success, -1 on parse error.
 */
int msg_parse(const uint8_t *buf, size_t len,
              int *type, char *name, size_t name_sz,
              uint16_t *port, char *payload, size_t payload_sz);

/*
 * Encode a message into buf.
 * Returns total bytes written, or -1 if buf_sz is too small.
 */
int msg_encode(uint8_t *buf, size_t buf_sz,
               int type, const char *name, uint16_t port,
               const char *payload);

/*
 * Convert a compact (pilcrow-delimited) payload to a multi-line program.
 * Splits on 0xB6, joins with '\n'.
 * Returns chars written (excluding NUL), or -1 on overflow.
 */
int msg_compact_to_program(const char *compact, char *out, size_t out_sz);

/*
 * Convert a multi-line program to compact form for UDP transmission.
 * Strips leading/trailing whitespace per line and comment lines (starting ';').
 * Joins non-empty lines with 0xB6.
 * Returns chars written (excluding NUL), or -1 on overflow.
 */
int msg_program_to_compact(const char *program, char *out, size_t out_sz);

#endif /* EDGE_MSG_H */
