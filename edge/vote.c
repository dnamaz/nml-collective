/*
 * vote.c — Two-phase VOTE/COMMIT aggregation for NML collective agents.
 */

#include "vote.h"

#include <string.h>

void vote_table_init(VoteTable *t)
{
    memset(t, 0, sizeof(*t));
}

/* Find session by hash; returns NULL if not present. */
static VoteSession *find_session(VoteTable *t, const char *phash)
{
    for (int i = 0; i < t->count; i++) {
        if (strncmp(t->sessions[i].phash, phash, 16) == 0) {
            return &t->sessions[i];
        }
    }
    return NULL;
}

int vote_add(VoteTable *t, const char *phash,
             const char *agent_name, float score,
             int quorum, time_t now)
{
    VoteSession *s = find_session(t, phash);

    if (!s) {
        if (t->count >= VOTE_MAX_SESSIONS) return -2;
        s = &t->sessions[t->count++];
        memset(s, 0, sizeof(*s));
        strncpy(s->phash, phash, 16);
        s->first_vote = now;
    }

    if (s->committed) return 1; /* already done */

    /* Duplicate vote check */
    for (int i = 0; i < s->count; i++) {
        if (strncmp(s->voters[i], agent_name, sizeof(s->voters[i]) - 1) == 0) {
            return -1;
        }
    }

    if (s->count >= VOTE_MAX_VOTERS) return -2;

    strncpy(s->voters[s->count], agent_name, sizeof(s->voters[s->count]) - 1);
    s->scores[s->count] = score;
    s->count++;

    if (s->count >= quorum) {
        float sum = 0.0f;
        for (int i = 0; i < s->count; i++) sum += s->scores[i];
        s->mean_score = sum / (float)s->count;
        s->committed  = 1;
        return 1;
    }

    return 0;
}

int vote_get_result(const VoteTable *t, const char *phash, float *mean_out)
{
    for (int i = 0; i < t->count; i++) {
        if (strncmp(t->sessions[i].phash, phash, 16) == 0 &&
            t->sessions[i].committed) {
            *mean_out = t->sessions[i].mean_score;
            return 0;
        }
    }
    return -1;
}

void vote_expire(VoteTable *t, time_t now, time_t max_age_seconds)
{
    int i = 0;
    while (i < t->count) {
        if (now - t->sessions[i].first_vote > max_age_seconds) {
            /* Compact: overwrite with last entry */
            t->sessions[i] = t->sessions[t->count - 1];
            t->count--;
        } else {
            i++;
        }
    }
}
