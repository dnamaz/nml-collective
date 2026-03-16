# Role: Enforcer

The immune system of the collective. The Enforcer monitors agent behavior, detects threats, quarantines compromised nodes, maintains ban lists, collects evidence, and gossips enforcement actions across the mesh. She acts on intelligence from the Oracle and enforces policy set by Sentients.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role enforcer` |
| Color | Red (`#da3633`) |
| Executes programs | No |
| Signs programs | No |
| Votes on data | No |
| Embeds Nebula | No |
| LLM | Not required |

## Three Enforcement Levels

### Level 1: Warning

Logged, visible in dashboard, auto-expires after 1 hour. No action taken.

Triggers:
- 3+ data rejections from the same source
- Score outlier with z-score > 2.0
- Name mismatch detected during health probe

### Level 2: Quarantine

Temporary isolation. The quarantined node's traffic is ignored by all peers — messages rejected, scores excluded from VOTE, data submissions auto-rejected. The node still runs but the mesh shuns it.

Triggers:
- 5+ data rejections in one hour (auto-quarantine)
- 3+ score outliers in one hour (auto-quarantine)
- Transaction chain integrity failure (immediate)
- Rate limit exceeded (>20 submissions per minute)
- Manual action by enforcer

Properties:
- Default duration: 1 hour
- Gossips to all peers (they start ignoring the node too)
- Sentient or enforcer can lift early
- Evidence preserved

### Level 3: Blacklist

Permanent ban. The node is removed from all peer lists and can never rejoin. Requires sentient approval — the enforcer can only propose.

Flow:
1. Enforcer calls `POST /blacklist` (proposes, quarantines immediately)
2. Sentient reviews evidence
3. Sentient calls `POST /blacklist/approve` (permanent)
4. Ban gossips to all peers
5. Only a sentient can remove via `POST /blacklist/remove`

## Trust Hierarchy

| Action | Who Can Do It | Who Can Undo It |
|--------|--------------|-----------------|
| Warn | Enforcer (auto or manual) | Auto-expires |
| Quarantine | Enforcer | Sentient, Enforcer |
| Blacklist | Enforcer proposes + Sentient approves | Sentient only |
| Quarantine the Enforcer | Sentient only | Sentient only |
| Quarantine a Sentient | **Not allowed** | — |

Enforcers cannot quarantine sentients. This prevents coups. If an enforcer goes rogue, the Oracle detects the anomalous enforcement pattern and a sentient can quarantine the enforcer.

## What Gets Quarantined

| Target | Effect |
|--------|--------|
| Worker | Scores excluded from VOTE, data auto-rejected |
| Oracle | Assessments ignored, but observation continues |
| Architect | Built programs rejected by sentient |
| Enforcer | Other enforcers' actions take precedence |
| **Sentient** | **Cannot be quarantined by enforcer** |

## Evidence Collection

Every enforcement action is backed by evidence:

```bash
curl "http://enforcer:9006/evidence?agent=worker_bad"
```

Evidence types:
- `data_rejected` — hash and rejection reason
- `score_outlier` — program hash, score, z-score
- `chain_failure` — integrity check detail
- `rate_violation` — submission count and window
- `impersonation` — name mismatch detail

## Gossip Protocol

Enforcement actions propagate across the mesh via `POST /enforce/receive`:

```json
{"type": "quarantine", "agent": "worker_bad", "reason": "...", "source": "guardian"}
```

Every agent that receives an enforcement message applies it locally — adding the target to their quarantine list. This means even agents without a local enforcer respect the ban.

## Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/threats` | GET | Full threat board (warnings, quarantined, blacklisted) |
| `/quarantine/node` | POST | Quarantine a node (enforcer only) |
| `/quarantine/lift` | POST | Lift quarantine (enforcer/sentient) |
| `/blacklist` | POST | Propose permanent blacklist (enforcer only) |
| `/blacklist/approve` | POST | Approve blacklist (sentient only) |
| `/evidence` | GET | Evidence log for a specific agent |
| `/enforce/receive` | POST | Receive enforcement gossip from peers |

## Dashboard View

The Enforcer dashboard is a security operations center:

- **Threat Board** — warnings (yellow), quarantined (red), blacklisted (dark red) with Lift buttons
- **Stats** — warning count, quarantined count, blacklisted count, monitor cycles
- **Nebula** — read-only view to monitor data flow
- **Oracle** — Ask panel for intelligence queries
- **Canvas** — red glow, "enforcing" label
- **Bottom bar** — hidden (enforcer doesn't execute or broadcast)

## Starting an Enforcer

```bash
python3 serve/nml_collective.py --name guardian --port 9006 \
    --seeds http://localhost:9001 --role enforcer
```

## Design Principle

The Enforcer is reactive, not preemptive. She doesn't decide policy (that's the sentient). She doesn't detect threats (that's the oracle). She **acts** on detected threats by isolating compromised nodes, preserving evidence, and protecting the mesh. Her authority is bounded — she can quarantine temporarily, but permanent bans require sentient approval. The collective's immune response is proportional, reversible, and auditable.
