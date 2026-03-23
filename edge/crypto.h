/*
 * NML Edge Worker — program signature verification.
 *
 * Wraps nml_crypto.h which supports dual-mode signing:
 *   HMAC-SHA256  — for keys shorter than 128 hex chars
 *   Ed25519      — for 128 hex char keys (via TweetNaCl)
 *
 * Signed programs start with:
 *   SIGN agent=<name> key=<hmac-sha256|ed25519>:<key_hex> sig=<sig_hex>
 *   <program body>
 */

#ifndef EDGE_CRYPTO_H
#define EDGE_CRYPTO_H

#include <stddef.h>

/*
 * Verify a signed NML program.
 *
 * Returns:
 *    0  — signature valid; *body_start points to the unsigned body
 *   -1  — not a signed program (no "SIGN " header); *body_start = program
 *   -2  — malformed header
 *   -3  — signature invalid; program must be rejected
 *
 * agent_out (size >= 64) receives the signer's agent name on success.
 */
int crypto_verify_program(const char *program, size_t len,
                           char *agent_out, size_t agent_sz,
                           const char **body_start);

/*
 * Compute a 16-char hex hash of program_body (first 8 bytes of SHA-256).
 * out must be at least 17 bytes.
 * Matches program_hash() in nml_collective.py (hexdigest[:16]).
 */
void crypto_program_hash(const char *program_body, char *out);

#endif /* EDGE_CRYPTO_H */
