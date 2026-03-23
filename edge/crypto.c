#include "crypto.h"

#include <stdlib.h>
#include <string.h>

/*
 * Pull in nml_crypto.h for SHA-256, HMAC-SHA256, and Ed25519 verify.
 * tweetnacl.c is compiled separately and linked in.
 */
#define NML_CRYPTO
#include "../../nml/runtime/nml_crypto.h"

int crypto_verify_program(const char *program, size_t len,
                           char *agent_out, size_t agent_sz,
                           const char **body_start)
{
    (void)len;

    if (strncmp(program, "SIGN ", 5) != 0) {
        *body_start = program;
        return -1; /* not signed — caller decides whether to accept */
    }

    return nml_verify_program(program, agent_out, agent_sz, body_start);
}

void crypto_program_hash(const char *program_body, char *out)
{
    uint8_t hash[32];
    sha256((const uint8_t *)program_body, strlen(program_body), hash);
    /* First 8 bytes → 16 hex chars, matching Python's hexdigest()[:16] */
    hex_encode(hash, 8, out);
    out[16] = '\0';
}
