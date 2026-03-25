/*
 * peer_table.h — Fixed-size peer registry for NML collective agents.
 *
 * Tracks active peers discovered via ANNOUNCE and HEARTBEAT messages.
 * Thread-unsafe: call only from the single event loop thread.
 */

#ifndef COLLECTIVE_PEER_TABLE_H
#define COLLECTIVE_PEER_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifndef COLLECTIVE_MAX_PEERS
#define COLLECTIVE_MAX_PEERS 64
#endif

typedef struct {
    char     name[64];
    char     ip[46];               /* sender IPv4/IPv6 string, "" if unknown */
    char     role[16];             /* "worker", "sentient", etc., or "" */
    uint16_t port;
    char     identity_payload[34]; /* "<mhash16>:<nid16>\0" or "" */
    time_t   last_seen;
    char     quarantine_reason[64]; /* "" = not quarantined */
    int      quarantined;
    int      active;
} PeerEntry;

typedef struct {
    PeerEntry entries[COLLECTIVE_MAX_PEERS];
    int       count; /* total slots used (active + quarantined) */
} PeerTable;

/* Initialise all slots to inactive. */
void peer_table_init(PeerTable *t);

/*
 * Add or refresh a peer.  Updates port, identity, and last_seen on revisit.
 * Returns a pointer to the entry, or NULL if the table is full and the peer
 * is not already present.
 */
/*
 * Add or refresh a peer.  Updates ip, port, identity, and last_seen on revisit.
 * ip may be NULL or "" if the sender address is not known.
 * Returns a pointer to the entry, or NULL if the table is full and the peer
 * is not already present.
 */
PeerEntry *peer_upsert(PeerTable *t, const char *name, const char *ip,
                       uint16_t port, const char *identity, time_t now);

/* Look up by name.  Returns NULL if not found / inactive. */
PeerEntry *peer_get(PeerTable *t, const char *name);

/* Mark a peer as quarantined.  Creates the entry if not present. */
void peer_quarantine(PeerTable *t, const char *name, const char *reason);

/*
 * Mark peers whose last_seen is older than stale_after seconds as inactive
 * (they are not quarantined, just dropped from the active set).
 */
void peer_sweep(PeerTable *t, time_t now, time_t stale_after);

/*
 * Serialise the peer table to a JSON array.
 * Returns chars written (excluding NUL), or -1 on truncation.
 */
int peer_list_json(const PeerTable *t, char *out, size_t out_sz);

#endif /* COLLECTIVE_PEER_TABLE_H */
