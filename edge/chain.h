/*
 * chain.h — Per-agent hash-linked transaction chain.
 *
 * Append-only binary log at <dir>/agents/<name>/chain.binlog providing
 * tamper-evident history. Each record chains to the previous one via an
 * 8-byte truncated SHA-256; rewriting any past record breaks the chain
 * for every record after it.
 *
 * Record format (35-byte fixed prefix + variable payload):
 *   [ 0.. 8)  tx_id        u64 BE, monotonic from 0
 *   [ 8..16)  timestamp    u64 BE, unix seconds
 *   [16..17)  tx_type      u8  (CHAIN_TX_*)
 *   [17..19)  payload_len  u16 BE
 *   [19..27)  prev_hash    8 bytes (all zero for tx_id = 0)
 *   [27..35)  hash         SHA-256(prev_hash || tx_id_BE || timestamp_BE
 *                                  || tx_type || payload_len_BE
 *                                  || payload)[0:8]
 *   [35..35 + payload_len)  payload
 *
 * Single-threaded access assumed. Agents run the main loop in one thread.
 */

#ifndef EDGE_CHAIN_H
#define EDGE_CHAIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CHAIN_TX_AGENT_JOIN    0x01
#define CHAIN_TX_DATA_SUBMIT   0x20
#define CHAIN_TX_DATA_APPROVE  0x21
#define CHAIN_TX_DATA_REJECT   0x22

#define CHAIN_PREFIX_LEN    35
#define CHAIN_HASH_LEN       8
#define CHAIN_MAX_PAYLOAD 4096

typedef struct {
    char     path[512];
    FILE    *f;                              /* open file, rb+ mode     */
    uint64_t next_tx_id;                     /* tx_id of the next write */
    uint8_t  last_hash[CHAIN_HASH_LEN];      /* hash of last record     */
    int      verified;                       /* 1 if last verify passed */
} Chain;

typedef struct {
    uint64_t tx_id;
    uint64_t timestamp;
    uint8_t  tx_type;
    uint16_t payload_len;
    uint8_t  prev_hash[CHAIN_HASH_LEN];
    uint8_t  hash[CHAIN_HASH_LEN];
    long     file_offset;                    /* byte offset of record start */
} ChainRecord;

/*
 * Open (creating if needed) the chain file.
 *
 * Existing chain: verifies integrity; if any record fails, returns -1 and,
 * if bad_tx_id is non-NULL via chain_verify, that field carries the failing
 * record. chain_open only returns 0/-1.
 *
 * Fresh chain: appends an AGENT_JOIN record with agent_name as payload.
 *
 * Returns 0 on success, -1 on any error.
 */
int chain_open(const char *dir, const char *agent_name, Chain *c);

/*
 * Close the chain. Safe on a zero-initialized Chain.
 */
void chain_close(Chain *c);

/*
 * Append a record. payload may be NULL iff payload_len == 0.
 * Durably flushed before returning.
 * Returns 0 on success, -1 on I/O error or invalid input.
 */
int chain_append(Chain *c, uint8_t tx_type,
                 const void *payload, size_t payload_len);

/*
 * Walk the entire chain, recomputing every hash.
 * If any record fails, writes its tx_id via *bad_tx_id (if non-NULL)
 * and returns -1. On success returns the record count (>= 0).
 */
int chain_verify(const Chain *c, uint64_t *bad_tx_id);

/*
 * Per-record callback used by chain_iter().
 */
typedef void (*chain_iter_cb)(const ChainRecord *rec,
                              const uint8_t *payload, void *ud);

/*
 * Iterate over every record, calling cb for each. Payload buffer passed to
 * cb is valid only for the duration of the call. Returns 0 on success,
 * -1 on I/O error.
 */
int chain_iter(const Chain *c, chain_iter_cb cb, void *ud);

#endif /* EDGE_CHAIN_H */
