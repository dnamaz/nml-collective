/*
 * peer_table.c — Fixed-size peer registry for NML collective agents.
 */

#include "peer_table.h"

#include <string.h>
#include <stdio.h>

void peer_table_init(PeerTable *t)
{
    memset(t, 0, sizeof(*t));
}

PeerEntry *peer_upsert(PeerTable *t, const char *name, const char *ip,
                       uint16_t port, const char *identity, time_t now)
{
    /* Search for existing entry */
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].active &&
            strncmp(t->entries[i].name, name, sizeof(t->entries[i].name) - 1) == 0) {
            t->entries[i].port      = port;
            t->entries[i].last_seen = now;
            if (ip && *ip) {
                strncpy(t->entries[i].ip, ip, sizeof(t->entries[i].ip) - 1);
                t->entries[i].ip[sizeof(t->entries[i].ip) - 1] = '\0';
            }
            if (identity && *identity) {
                strncpy(t->entries[i].identity_payload, identity,
                        sizeof(t->entries[i].identity_payload) - 1);
                t->entries[i].identity_payload[sizeof(t->entries[i].identity_payload) - 1] = '\0';
            }
            return &t->entries[i];
        }
    }

    /* New entry */
    if (t->count >= COLLECTIVE_MAX_PEERS) return NULL;

    PeerEntry *e = &t->entries[t->count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    if (ip && *ip)
        strncpy(e->ip, ip, sizeof(e->ip) - 1);
    e->port      = port;
    e->last_seen = now;
    e->active    = 1;
    if (identity && *identity) {
        strncpy(e->identity_payload, identity, sizeof(e->identity_payload) - 1);
    }
    return e;
}

PeerEntry *peer_get(PeerTable *t, const char *name)
{
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].active &&
            strncmp(t->entries[i].name, name, sizeof(t->entries[i].name) - 1) == 0) {
            return &t->entries[i];
        }
    }
    return NULL;
}

void peer_quarantine(PeerTable *t, const char *name, const char *reason)
{
    /* Update existing entry if present */
    for (int i = 0; i < t->count; i++) {
        if (strncmp(t->entries[i].name, name, sizeof(t->entries[i].name) - 1) == 0) {
            t->entries[i].quarantined = 1;
            t->entries[i].active      = 0;
            strncpy(t->entries[i].quarantine_reason, reason,
                    sizeof(t->entries[i].quarantine_reason) - 1);
            t->entries[i].quarantine_reason[sizeof(t->entries[i].quarantine_reason) - 1] = '\0';
            return;
        }
    }

    /* Create a quarantined-only entry */
    if (t->count >= COLLECTIVE_MAX_PEERS) return;
    PeerEntry *e = &t->entries[t->count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->quarantined = 1;
    e->active      = 0;
    strncpy(e->quarantine_reason, reason, sizeof(e->quarantine_reason) - 1);
    e->quarantine_reason[sizeof(e->quarantine_reason) - 1] = '\0';
}

void peer_sweep(PeerTable *t, time_t now, time_t stale_after)
{
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].active && !t->entries[i].quarantined) {
            if (now - t->entries[i].last_seen > stale_after) {
                t->entries[i].active = 0;
            }
        }
    }
}

/* Minimal JSON string escape — replaces " and \ only */
static int json_str(char *out, size_t rem, const char *s)
{
    int written = 0;
    if (rem < 3) return -1;
    out[written++] = '"';
    for (; *s && (size_t)written < rem - 2; s++) {
        if (*s == '"' || *s == '\\') {
            if ((size_t)written + 1 >= rem - 2) break;
            out[written++] = '\\';
        }
        out[written++] = *s;
    }
    out[written++] = '"';
    out[written]   = '\0';
    return written;
}

int peer_list_json(const PeerTable *t, char *out, size_t out_sz)
{
    int pos = 0;

#define EMIT(s) do { \
    size_t _l = strlen(s); \
    if ((size_t)pos + _l >= out_sz) return -1; \
    memcpy(out + pos, s, _l); pos += (int)_l; \
} while(0)

#define EMIT_STR(s) do { \
    char _tmp[256]; \
    int _r = json_str(_tmp, sizeof(_tmp), s); \
    if (_r < 0) return -1; \
    if ((size_t)pos + (size_t)_r >= out_sz) return -1; \
    memcpy(out + pos, _tmp, (size_t)_r); pos += _r; \
} while(0)

    EMIT("[");
    int first = 1;
    for (int i = 0; i < t->count; i++) {
        const PeerEntry *e = &t->entries[i];
        if (!e->active && !e->quarantined) continue;

        if (!first) EMIT(",");
        first = 0;

        EMIT("{\"name\":");
        EMIT_STR(e->name);

        EMIT(",\"ip\":");
        EMIT_STR(e->ip);

        EMIT(",\"role\":");
        EMIT_STR(e->role);

        char tmp[64];
        snprintf(tmp, sizeof(tmp), ",\"port\":%u", e->port);
        EMIT(tmp);

        EMIT(",\"identity\":");
        EMIT_STR(e->identity_payload);

        snprintf(tmp, sizeof(tmp), ",\"active\":%s,\"quarantined\":%s",
                 e->active ? "true" : "false",
                 e->quarantined ? "true" : "false");
        EMIT(tmp);

        if (e->quarantined) {
            EMIT(",\"reason\":");
            EMIT_STR(e->quarantine_reason);
        }

        snprintf(tmp, sizeof(tmp), ",\"last_seen\":%ld}", (long)e->last_seen);
        EMIT(tmp);
    }
    EMIT("]");
    out[pos] = '\0';
    return pos;

#undef EMIT
#undef EMIT_STR
}
