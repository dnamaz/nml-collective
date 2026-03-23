/*
 * vote.h — Two-phase VOTE/COMMIT aggregation for NML collective agents.
 *
 * Collects MSG_RESULT payloads ("<hash16>:<score>") from peer agents.
 * When the number of votes for a program hash reaches quorum, the mean
 * score is computed and the entry is marked committed.
 *
 * Matching nml_collective.py: quorum is ceil(active_peers / 2) ≥ 1.
 * Callers pass explicit quorum to avoid needing a peer table reference here.
 */

#ifndef COLLECTIVE_VOTE_H
#define COLLECTIVE_VOTE_H

#include <stddef.h>
#include <time.h>

#ifndef VOTE_MAX_SESSIONS  /* max concurrent program hashes in flight */
#define VOTE_MAX_SESSIONS 32
#endif

#ifndef VOTE_MAX_VOTERS    /* max votes per session */
#define VOTE_MAX_VOTERS 64
#endif

typedef struct {
    char  phash[17];                         /* program hash (16 hex + NUL) */
    char  voters[VOTE_MAX_VOTERS][64];       /* agent names that voted */
    float scores[VOTE_MAX_VOTERS];
    int   count;
    int   committed;
    float mean_score;
    time_t first_vote;
} VoteSession;

typedef struct {
    VoteSession sessions[VOTE_MAX_SESSIONS];
    int         count;
} VoteTable;

/* Initialise the vote table. */
void vote_table_init(VoteTable *t);

/*
 * Record a vote from agent_name for program phash with the given score.
 * quorum is the minimum vote count to declare a result (pass 1 for immediate).
 *
 * Returns:
 *   1  — quorum reached; call vote_get_result() to retrieve mean_score
 *   0  — vote recorded, quorum not yet reached
 *  -1  — duplicate vote from same agent (ignored)
 *  -2  — table full (session dropped)
 */
int vote_add(VoteTable *t, const char *phash,
             const char *agent_name, float score,
             int quorum, time_t now);

/*
 * Retrieve the committed mean score for phash.
 * Returns 0 and sets *mean_out on success; -1 if not committed/not found.
 */
int vote_get_result(const VoteTable *t, const char *phash, float *mean_out);

/*
 * Expire sessions older than max_age_seconds.
 * Call periodically to reclaim slots.
 */
void vote_expire(VoteTable *t, time_t now, time_t max_age_seconds);

#endif /* COLLECTIVE_VOTE_H */
