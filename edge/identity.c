/*
 * identity.c — Tamper-evident node identity for NML Edge Worker.
 *
 * See identity.h for the full description.
 *
 * SHA-256 is provided by nml_crypto.h (same include pattern as crypto.c).
 * No dynamic allocation; all buffers are caller-supplied.
 */

#include "identity.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Pull in SHA-256 and hex_encode as static functions.
   stdlib.h must come first — nml_crypto.h uses malloc/free for Ed25519. */
#define NML_CRYPTO
#include "../../nml/runtime/nml_crypto.h"

/* ── Platform UID reading ─────────────────────────────────────────────────── */

int identity_get_hw_uid(uint8_t *buf, size_t *len, size_t buf_capacity)
{
#if defined(__arm__) || defined(__ARM_ARCH)
    /* ARM: delegate to BSP hook (weak symbol below) */
    *len = buf_capacity;
    return arm_edge_hardware_uid(buf, len);

#elif defined(__linux__)
    /* Linux: read /etc/machine-id (32-char hex UUID, newline-terminated) */
    FILE *f = fopen("/etc/machine-id", "r");
    if (!f) {
        /* Fallback: /proc/sys/kernel/random/boot_id */
        f = fopen("/proc/sys/kernel/random/boot_id", "r");
    }
    if (f) {
        size_t n = fread(buf, 1, buf_capacity - 1, f);
        fclose(f);
        /* Strip trailing whitespace / newlines */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                          buf[n-1] == ' '  || buf[n-1] == '\t')) {
            n--;
        }
        if (n > 0) {
            *len = n;
            return 0;
        }
    }
    return -1;   /* fallback to compile-time constant handled by identity_init */

#else
    /* macOS / unknown: use hostname */
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
        size_t hn = strlen(hostname);
        if (hn > buf_capacity - 1) hn = buf_capacity - 1;
        memcpy(buf, hostname, hn);
        *len = hn;
        return 0;
    }
    return -1;
#endif
}

/* ── Hash derivation ──────────────────────────────────────────────────────── */

void identity_machine_hash(const uint8_t *hw_uid, size_t hw_uid_len,
                            char *out_hex, uint8_t *out_bytes)
{
    uint8_t full[32];
    sha256(hw_uid, hw_uid_len, full);
    memcpy(out_bytes, full, 8);
    hex_encode(full, 8, out_hex);
    out_hex[16] = '\0';
}

void identity_node_id(const uint8_t *machine_hash_bytes,
                      const char *agent_name,
                      char *out_hex)
{
    /* node_id = SHA-256(machine_hash_bytes[8] || ':' || agent_name)[0:8] */
    SHA256_CTX ctx;
    uint8_t full[32];
    const uint8_t colon = ':';

    sha256_init(&ctx);
    sha256_update(&ctx, machine_hash_bytes, 8);
    sha256_update(&ctx, &colon, 1);
    sha256_update(&ctx, (const uint8_t *)agent_name, strlen(agent_name));
    sha256_final(&ctx, full);

    hex_encode(full, 8, out_hex);
    out_hex[16] = '\0';
}

void identity_payload(const char *machine_hash_hex, const char *node_id_hex,
                      char *out)
{
    memcpy(out,      machine_hash_hex, 16);
    out[16] = ':';
    memcpy(out + 17, node_id_hex,      16);
    out[33] = '\0';
}

/* ── One-shot initialisation ──────────────────────────────────────────────── */

void identity_init(const char *agent_name,
                   char    *machine_hash_hex_out,
                   uint8_t *machine_hash_bytes_out,
                   char    *node_id_hex_out,
                   char    *payload_out)
{
    uint8_t uid_buf[256];
    size_t  uid_len = sizeof(uid_buf);

    int ok = identity_get_hw_uid(uid_buf, &uid_len, sizeof(uid_buf));
    if (ok < 0) {
        /* Use compile-time fallback */
        const char *fallback = EDGE_MACHINE_ID_STR;
        uid_len = strlen(fallback);
        if (uid_len > sizeof(uid_buf) - 1) uid_len = sizeof(uid_buf) - 1;
        memcpy(uid_buf, fallback, uid_len);
        fprintf(stderr, "[identity] WARNING: no hardware UID available, "
                "using compile-time default '%s'\n", fallback);
    }

    identity_machine_hash(uid_buf, uid_len,
                          machine_hash_hex_out, machine_hash_bytes_out);
    identity_node_id(machine_hash_bytes_out, agent_name, node_id_hex_out);
    identity_payload(machine_hash_hex_out, node_id_hex_out, payload_out);
}

/* ── ARM BSP hook (weak default) ─────────────────────────────────────────── */

__attribute__((weak))
int arm_edge_hardware_uid(uint8_t *buf, size_t *len)
{
    const char *s = EDGE_MACHINE_ID_STR;
    size_t sl = strlen(s);
    if (sl > *len) sl = *len;
    memcpy(buf, s, sl);
    *len = sl;
    return 0;
}
