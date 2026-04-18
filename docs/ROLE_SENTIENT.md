# Role: Sentient

The central will of the collective. While every agent votes and executes, the Sentient is the only role that can *commit* — it signs programs, finalizes data out of quarantine, and owns the Nebula ledger that all other agents read from.

The rest of the collective can observe, deliberate, and vote indefinitely, but nothing becomes durable without a Sentient. It is the consensus finalizer and the identity anchor: the Collective's trust root and continuity persist through the Sentient's key material even as other nodes join and leave.

Pipeline position:

```
Oracle specs → Architect builds → Sentient signs → Workers execute → VOTE → Sentient approves data
```

Special protections that reflect this role's centrality:
- Enforcers **cannot quarantine** a Sentient
- Blacklist decisions require **Sentient approval** (`--approve` flag)
- Only Sentients hold Ed25519 **private keys** — all other roles verify only

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role sentient` |
| Color | Purple (`#d2a8ff`) |
| Executes programs | Yes |
| Signs programs | Yes (Ed25519) |
| Votes on data | Yes (authority weight) |
| Embeds Nebula | Yes (persistent storage) |

## Responsibilities

### 1. Program Signing

Sentients are the only role that can sign NML programs with Ed25519:

```bash
nml-crypto --sign program.nml --key private_key --agent prime
```

The private key stays local. Only the public key appears in the SIGN header. Any agent can verify, but only sentients can sign.

### 2. Data Approval

When the Custodian ingests data (on behalf of operators, workers, or external sources), it enters quarantine. Sentients review and vote:

- **Approve** — data moves to the pool and feeds programs
- **Reject** — data stays in quarantine as a negative example

With multiple sentients, quorum rules apply (default: majority). Oracle votes count toward quorum when combined with at least one sentient vote.

### 3. Nebula Ownership

Sentients embed a local Nebula instance with persistent three-layer storage:

- **Layer 1 (Truth):** Binary tensor objects + per-agent transaction chains
- **Layer 2 (Speed):** SQLite indexes for fast queries
- **Layer 3 (Intelligence):** 64-dim vector embeddings for semantic search

All data flows through the sentient's Nebula. Workers submit to it, oracles read from it.

### 4. Execution

Sentients also execute programs when they have local data. Their scores participate in VOTE consensus alongside workers.

## Endpoints (Sentient-Specific)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/data/approve` | POST | Vote to approve quarantined data |
| `/data/reject` | POST | Vote to reject quarantined data |
| `/data/quarantine` | GET | List pending quarantine items |
| `/data/pool` | GET | Approved data pool |
| `/nebula/stats` | GET | Nebula storage statistics |

## Dashboard View

The sentient dashboard emphasizes Nebula management:

- **Nebula section prominent** — programs, approved/rejected data, quarantine pending, executions, ledger entries
- **Quarantine inbox** — pending items with Approve/Reject buttons, Oracle analysis displayed alongside each item
- **My Results** — execution scores and consensus bar
- **Bottom bar** — Broadcast Program, VOTE Consensus

## Quorum Rules

```
Single sentient:  1 approve = promoted
Two sentients:    Both must approve
Three sentients:  2 of 3 must approve
Oracle vote:      Counts as +1 when ≥1 sentient has also approved
```

## Starting a Sentient

```bash
python3 serve/nml_collective.py --name prime --port 9001 --role sentient --data demos/agent1.nml.data
```

The Nebula storage initializes at `.nebula/` relative to the project root.
