/*
 * ledger_http.c — `/ledger` and `/ledger/verify` handlers.
 *
 * See ledger_http.h for API. Extracted verbatim from the duplicated copies
 * that were in sentient_agent.c and custodian_agent.c.
 */

#define _POSIX_C_SOURCE 200809L

#include "ledger_http.h"
#include "http_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal: per-call output buffer / iteration state ─────────────── */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    seen;
    int    emitted;
    int    offset;
    int    limit;
    int    first;
} LedgerCtx;

static const char *tx_type_name(uint8_t t)
{
    switch (t) {
        case CHAIN_TX_AGENT_JOIN:   return "AGENT_JOIN";
        case CHAIN_TX_DATA_SUBMIT:  return "DATA_SUBMIT";
        case CHAIN_TX_DATA_APPROVE: return "DATA_APPROVE";
        case CHAIN_TX_DATA_REJECT:  return "DATA_REJECT";
        default:                    return "UNKNOWN";
    }
}

static int ledger_reserve(LedgerCtx *ctx, size_t extra)
{
    if (ctx->len + extra + 4 <= ctx->cap) return 0;
    size_t ncap = ctx->cap;
    while (ctx->len + extra + 4 > ncap) {
        size_t next = ncap * 2;
        if (next > (4u * 1024u * 1024u)) next = 4u * 1024u * 1024u;
        if (next == ncap) return -1;
        ncap = next;
    }
    char *nb = (char *)realloc(ctx->buf, ncap);
    if (!nb) return -1;
    ctx->buf = nb;
    ctx->cap = ncap;
    return 0;
}

static void ledger_append_escaped(LedgerCtx *ctx,
                                  const uint8_t *src, size_t len)
{
    if (ledger_reserve(ctx, len * 2 + 2) < 0) return;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == '"' || c == '\\') {
            ctx->buf[ctx->len++] = '\\';
            ctx->buf[ctx->len++] = (char)c;
        } else if (c < 0x20 || c > 0x7e) {
            ctx->buf[ctx->len++] = '?';
        } else {
            ctx->buf[ctx->len++] = (char)c;
        }
    }
}

static void iter_cb(const ChainRecord *rec,
                    const uint8_t *payload, void *ud)
{
    LedgerCtx *ctx = (LedgerCtx *)ud;
    int idx = ctx->seen++;
    if (idx < ctx->offset)          return;
    if (ctx->emitted >= ctx->limit) return;

    char head[160];
    int n = snprintf(head, sizeof(head),
        "%s{\"tx_id\":%llu,\"timestamp\":%llu,"
        "\"tx_type\":\"%s\",\"payload\":\"",
        ctx->first ? "" : ",",
        (unsigned long long)rec->tx_id,
        (unsigned long long)rec->timestamp,
        tx_type_name(rec->tx_type));
    if (n < 0) return;
    if (ledger_reserve(ctx, (size_t)n) < 0) return;
    memcpy(ctx->buf + ctx->len, head, (size_t)n);
    ctx->len += (size_t)n;

    ledger_append_escaped(ctx, payload, rec->payload_len);

    if (ledger_reserve(ctx, 2) < 0) return;
    ctx->buf[ctx->len++] = '"';
    ctx->buf[ctx->len++] = '}';

    ctx->first = 0;
    ctx->emitted++;
}

/* ── ledger_http_serve_index ────────────────────────────────────────── */

void ledger_http_serve_index(compat_socket_t fd,
                             Chain *chain,
                             const char *query)
{
    int offset = 0;
    int limit  = 100;
    if (query) {
        const char *op = strstr(query, "offset=");
        const char *lp = strstr(query, "limit=");
        if (op) offset = atoi(op + 7);
        if (lp) limit  = atoi(lp + 6);
        if (limit > 1000) limit = 1000;
        if (limit < 0)    limit = 0;
        if (offset < 0)   offset = 0;
    }

    LedgerCtx ctx;
    ctx.cap     = 16384;
    ctx.len     = 0;
    ctx.seen    = 0;
    ctx.emitted = 0;
    ctx.offset  = offset;
    ctx.limit   = limit;
    ctx.first   = 1;
    ctx.buf     = (char *)malloc(ctx.cap);
    if (!ctx.buf) {
        http_send_json(fd, 500, "{\"error\":\"oom\"}");
        return;
    }
    ctx.buf[ctx.len++] = '[';

    chain_iter(chain, iter_cb, &ctx);

    if (ctx.len + 2 > ctx.cap) {
        char *nb = (char *)realloc(ctx.buf, ctx.cap + 16);
        if (nb) { ctx.buf = nb; ctx.cap += 16; }
    }
    if (ctx.len + 1 < ctx.cap) ctx.buf[ctx.len++] = ']';

    /* Pack as binary-safe body — http_send_binary honors exact length. */
    http_send_binary(fd, "application/json", ctx.buf, ctx.len);
    free(ctx.buf);
}

/* ── ledger_http_serve_verify ──────────────────────────────────────── */

void ledger_http_serve_verify(compat_socket_t fd, Chain *chain)
{
    uint64_t bad = 0;
    int count = chain_verify(chain, &bad);

    char body[128];
    if (count < 0) {
        snprintf(body, sizeof(body),
            "{\"ok\":false,\"bad_tx_id\":%llu}",
            (unsigned long long)bad);
    } else {
        snprintf(body, sizeof(body),
            "{\"ok\":true,\"total\":%d}", count);
    }
    http_send_json(fd, 200, body);
}
