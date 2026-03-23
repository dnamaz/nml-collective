/*
 * identity.h — Tamper-evident node identity for NML Edge Worker.
 *
 * Each worker derives two stable 8-byte (16 hex char) values at startup:
 *
 *   machine_hash = SHA-256(hw_uid)[0:8]
 *   node_id      = SHA-256(machine_hash_bytes || ':' || agent_name)[0:8]
 *
 * Both are carried in MSG_ANNOUNCE and MSG_HEARTBEAT as the 33-byte payload:
 *
 *   "<machine_hash16>:<node_id16>"
 *
 * The Enforcer recomputes node_id from the received machine_hash + agent_name
 * and quarantines any node where the values don't match.  This binds each
 * agent name to a specific machine (SHA-256 preimage resistance prevents
 * forging a matching node_id for a different name or machine).
 *
 * Platform UID sources (in priority order):
 *   Linux  : /etc/machine-id
 *   ARM    : arm_edge_hardware_uid() — weak BSP hook, user overrides
 *   macOS  : gethostname() (development / CI)
 *   Fallback: EDGE_MACHINE_ID_STR compile-time constant
 */

#ifndef EDGE_IDENTITY_H
#define EDGE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/* ── Platform UID ─────────────────────────────────────────────────────────── */

/*
 * Read the platform hardware UID into buf (up to buf_capacity bytes).
 * Sets *len to the number of bytes written.
 * Returns 0 on success, -1 if the platform source is unavailable.
 * On -1 the caller should use EDGE_MACHINE_ID_STR as a fallback.
 */
int identity_get_hw_uid(uint8_t *buf, size_t *len, size_t buf_capacity);

/* ── Hash derivation ──────────────────────────────────────────────────────── */

/*
 * Derive machine_hash from a hardware UID byte array.
 *
 *   machine_hash = SHA-256(hw_uid_bytes, hw_uid_len)[0:8]
 *
 * out_hex   : 17-byte buffer → 16 hex chars + NUL
 * out_bytes :  8-byte buffer → raw hash bytes (input to node_id derivation)
 */
void identity_machine_hash(const uint8_t *hw_uid, size_t hw_uid_len,
                            char    *out_hex,    /* 17 bytes */
                            uint8_t *out_bytes); /*  8 bytes */

/*
 * Derive node_id from machine_hash bytes and agent_name.
 *
 *   node_id = SHA-256(machine_hash_bytes[8] || ':' || agent_name)[0:8]
 *
 * out_hex : 17-byte buffer → 16 hex chars + NUL
 */
void identity_node_id(const uint8_t *machine_hash_bytes, /* 8 bytes */
                      const char    *agent_name,
                      char          *out_hex);           /* 17 bytes */

/*
 * Format the identity payload string.
 *
 *   out : "<machine_hash16>:<node_id16>\0"  (34-byte buffer minimum)
 */
void identity_payload(const char *machine_hash_hex,  /* 16 chars */
                      const char *node_id_hex,        /* 16 chars */
                      char       *out);               /* 34 bytes */

/* ── One-shot initialisation ──────────────────────────────────────────────── */

/*
 * Derive all identity fields from the current platform and agent_name.
 * Call once at startup before the first MSG_ANNOUNCE.
 *
 * agent_name          : the agent's name string (used in node_id derivation)
 * machine_hash_hex_out: 17-byte buffer → 16-char hex machine hash + NUL
 * machine_hash_bytes_out: 8-byte buffer → raw machine hash bytes
 * node_id_hex_out     : 17-byte buffer → 16-char hex node id + NUL
 * payload_out         : 34-byte buffer → "<mhash16>:<nid16>\0"
 */
void identity_init(const char *agent_name,
                   char    *machine_hash_hex_out,     /* 17 bytes */
                   uint8_t *machine_hash_bytes_out,   /*  8 bytes */
                   char    *node_id_hex_out,          /* 17 bytes */
                   char    *payload_out);             /* 34 bytes */

/* ── ARM BSP hook ─────────────────────────────────────────────────────────── */

/*
 * Override this in your BSP to supply the CPU hardware UID.
 * The default weak implementation returns EDGE_MACHINE_ID_STR.
 *
 * Example for STM32 (UID at 0x1FFF7A10, 12 bytes):
 *   int arm_edge_hardware_uid(uint8_t *buf, size_t *len) {
 *       const uint32_t *uid = (const uint32_t *)0x1FFF7A10;
 *       memcpy(buf, uid, 12);
 *       *len = 12;
 *       return 0;
 *   }
 */
int arm_edge_hardware_uid(uint8_t *buf, size_t *len);

#endif /* EDGE_IDENTITY_H */
