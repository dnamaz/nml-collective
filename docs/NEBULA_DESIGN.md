# Nebula — The Collective's Brain

## Overview

The Nebula is the shared memory, data store, and coordination layer at the center of the NML Collective. It holds signed programs, approved data, execution results, and consensus — while agents orbit around it, contributing observations and computation.

The Nebula is not a single server. It's a distributed ledger replicated across sentient agents, with workers contributing data and compute.

```mermaid
flowchart TB
    subgraph nebula [Nebula — Shared Memory]
        programs["Programs\n(signed, immutable)"]
        quarantine["Quarantine\n(unverified data)"]
        pool["Data Pool\n(approved batches)"]
        execlog["Execution Log\n(append-only)"]
        consensus["Consensus\n(VOTE results)"]
    end
    subgraph sentients [Sentients — Authorities]
        S1["Sentient A"]
        S2["Sentient B"]
    end
    subgraph workers [Workers — Compute]
        W1["Worker 1"]
        W2["Worker 2"]
        W3["Worker 3"]
    end
    S1 -->|sign programs| programs
    S1 -->|approve data| quarantine
    S2 -->|approve data| quarantine
    quarantine -->|approved| pool
    W1 -->|submit data| quarantine
    W2 -->|submit data| quarantine
    W3 -->|submit data| quarantine
    pool -->|feeds| W1
    pool -->|feeds| W2
    programs -->|broadcast| W1
    programs -->|broadcast| W2
    programs -->|broadcast| W3
    W1 -->|results| execlog
    W2 -->|results| execlog
    W3 -->|results| execlog
    execlog -->|VOTE| consensus
```

---

## Design Decision 1: Who Writes

### Roles

**Sentients** — the authorities of the collective.
- Sign and publish programs to the ledger
- Approve or reject data in quarantine via VOTE
- Define data manifests (what data feeds which program)
- Can promote/demote workers

**Workers** — the compute nodes.
- Submit observed data to quarantine (never directly to the ledger)
- Execute approved programs with approved data
- Report execution results
- Cannot modify programs or approve their own submissions

### Write Flow

```mermaid
sequenceDiagram
    participant W as Worker
    participant Q as Quarantine
    participant S1 as Sentient A
    participant S2 as Sentient B
    participant L as Ledger

    W->>Q: Submit @transactions_batch_47 (500 rows)
    Q->>S1: Notification: new data pending
    Q->>S2: Notification: new data pending
    S1->>Q: APPROVE (schema valid, distribution normal)
    S2->>Q: APPROVE (no anomalies detected)
    Note over Q: Quorum met (2/2)
    Q->>L: Promote to Data Pool
    L->>W: Trigger re-execution of dependent programs
```

### Quarantine

All worker-submitted data enters quarantine — untrusted by default. Data sits in quarantine until sentients vote on it.

```
Quarantine Entry {
  hash:          content-addressed SHA-256
  submitted_by:  worker name + signature
  submitted_at:  timestamp
  content:       @name + shape + data
  status:        pending | approved | rejected | unreviewed
  votes:         [{sentient, vote, reason}]
  auto_checks:   schema_match, bounds_check, null_check
  policy:        {quorum: majority, expires: 3600s}
}
```

**Auto-checks** run immediately on submission:
- Schema matches what the program expects
- Values within expected bounds
- No nulls or NaN

Auto-checks that pass fast-track sentient review. Auto-checks that fail can auto-reject.

---

## Design Decision 2: Data Is Additive

### No Conflicts, No Merging

Two workers submitting different data is not a conflict — it's more data. The collective doesn't need agreement on data. It needs agreement on results.

```
Worker A submits: @transactions_us_east     (500 rows)
Worker B submits: @transactions_europe      (300 rows)
Worker C submits: @transactions_asia        (200 rows)

These are three separate contributions.
No merge. No ownership. No conflict.
```

### Diverse Perspectives Strengthen Consensus

Each agent trains on different data. Each produces a different score. VOTE combines diverse perspectives into a consensus that's stronger than any individual:

```
Program: fraud_detection.nml (same for everyone, signed)
Agent A: trains on US data     → score 0.73
Agent B: trains on EU data     → score 0.68
Agent C: trains on Asia data   → score 0.81
VOTE median: 0.73

The diversity IS the value.
```

### Data Manifests

When a sentient wants specific data to feed a program, they create a signed manifest:

```
Manifest {
  program:         sha256:958c21...
  signed_by:       sentient_A
  data_bindings: {
    @training_data:   [batch_12, batch_13, batch_14]  (CONCAT)
    @training_labels: [labels_12, labels_13, labels_14]
    @new_transaction: latest_flagged
  }
}
```

Workers receive the manifest, resolve references from the data pool, and execute. The program never changes — only the manifest changes.

---

## Design Decision 3: Nothing Expires

### Classify, Don't Delete

Data doesn't expire. It gets classified:

```mermaid
flowchart TD
    submitted["SUBMITTED\nWorker pushes a batch"]
    quarantine["QUARANTINED\nAwaiting sentient review"]
    approved["APPROVED\nTrusted, feeds programs"]
    rejected["REJECTED\nFailed verification\nKept as negative examples"]
    unreviewed["UNREVIEWED\nNo sentient voted in time\nCan be re-submitted"]
    superseded["SUPERSEDED\nNewer batch covers same period\nKept for history"]

    submitted --> quarantine
    quarantine --> approved
    quarantine --> rejected
    quarantine --> unreviewed
    approved --> superseded
```

### Bad Data Trains the Guards

Rejected data is not garbage — it's a signal:

- A poisoning attempt teaches the collective what attacks look like
- A malfunctioning sensor's output trains anomaly detection
- Distribution anomalies become features for the quarantine auto-checker

The collective can train a meta-program:

```
anomaly_detector.nml
  Trained on: REJECTED batches (positive) + APPROVED batches (negative)
  Purpose: auto-flag suspicious submissions in quarantine
  Becomes: an automated sentient
```

The collective learns from its own rejection history. Every bad submission makes quarantine smarter.

### Storage Is Not a Problem

```
Per batch:     ~10 KB (500 rows, 6 features)
Per day:       100 batches = 1 MB
Per year:      365 MB
Per decade:    3.6 GB

Programs:      340 bytes each
Consensus:     < 1 KB each
Exec history:  200 bytes per execution
```

A Raspberry Pi can hold the full ledger for years. Tiered storage (hot/warm/cold) handles growth if needed, but deletion is never required.

### The Principle

**The collective never forgets. It classifies.**

Good data trains the models. Bad data trains the guards. Old data provides context. Every rejection makes the system smarter.

---

## Design Decision 4: Roles Are the Incentive

### No Tokens, No Credits, No Marketplace

A worker doesn't need a reward for executing programs any more than a neuron needs a reward for firing. It's what it does. The collective is an organism, not a marketplace.

- A sentient approves because that's its function
- A worker computes because that's its function
- The nebula stores because that's its function

If a component stops doing its job, the collective detects it (heartbeat failure) and compensates (other agents pick up the work). There's no punishment — just natural selection. Healthy agents persist; dead agents are forgotten.

### Health Over Incentives

Instead of rewards, monitor health:

```
Sentient health:
  - Review latency (how fast do they vote on quarantine?)
  - Approval rate (are they rubber-stamping or actually reviewing?)
  - Uptime

Worker health:
  - Submission quality (what % of their data passes auto-checks?)
  - Execution reliability (do they complete programs?)
  - Heartbeat consistency
  - Agreement with consensus (do their scores align with VOTE?)
```

Workers that consistently disagree with consensus may be malfunctioning. Sentients that never reject anything aren't adding value. The collective tracks health, not balance sheets.

---

## Ledger Structure

Every object in the nebula is content-addressed (hash = identity):

```
Ledger Entry {
  type:       program | data | execution | consensus | manifest
  hash:       SHA-256 of content
  prev_hash:  chain integrity (optional, for ordering)
  timestamp:  when created
  author:     agent name + Ed25519 public key
  signature:  Ed25519 of (hash + timestamp + content)
  status:     approved | rejected | pending | superseded
  content:    the payload
}
```

The ledger is **append-only**. Nothing is modified or deleted. New entries reference old ones (prev_hash) to maintain ordering and integrity.

---

## Implementation Plan

### Phase 1: Nebula Core (`serve/nml_nebula.py`)
- Content-addressed store (programs, data, executions, consensus)
- Quarantine with auto-checks
- Approval VOTE endpoint for sentients
- Data pool queries
- Execution trigger on data approval

### Phase 2: Agent Roles
- `--role sentient` and `--role worker` flags
- Workers: `/submit-data` → quarantine
- Sentients: `/approve` and `/reject` with reasons
- Manifests: sentient signs data bindings for a program

### Phase 3: Re-execution
- When new data is approved, trigger re-execution of dependent programs
- Agents pull program from cache + data from pool
- New scores → new VOTE → updated consensus

### Phase 4: Dashboard
- Nebula visualization shows quarantine inbox, data pool, execution timeline
- Sentient view: pending approvals, vote buttons
- Worker view: submission status, execution queue
