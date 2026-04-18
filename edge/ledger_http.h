/*
 * ledger_http.h — `/ledger` + `/ledger/verify` HTTP handlers.
 *
 * Factored out of sentient_agent.c and custodian_agent.c, which previously
 * carried verbatim copies. Any role that owns a `Chain` can plug these in
 * directly.
 */

#ifndef EDGE_LEDGER_HTTP_H
#define EDGE_LEDGER_HTTP_H

#include "compat.h"
#include "chain.h"

/*
 * Serve GET /ledger[?offset=N&limit=M]. Iterates the chain and emits a JSON
 * array of records, one per record: {tx_id, timestamp, tx_type, payload}.
 * Payload bytes are JSON-string-escaped (quote, backslash, non-printable → ?).
 *
 * `query` is the raw query string after '?' (may be NULL for no params).
 * Defaults: offset=0, limit=100 (capped at 1000).
 */
void ledger_http_serve_index(compat_socket_t fd,
                             Chain *chain,
                             const char *query);

/*
 * Serve GET /ledger/verify. Runs chain_verify on the given chain.
 * Returns JSON:
 *   {"ok": true, "total": N}       on clean chain
 *   {"ok": false, "bad_tx_id": N}  on integrity failure
 */
void ledger_http_serve_verify(compat_socket_t fd, Chain *chain);

#endif /* EDGE_LEDGER_HTTP_H */
