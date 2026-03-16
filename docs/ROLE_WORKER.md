# Role: Worker

The muscle of the collective. Workers execute programs on their local data, submit data observations to quarantine, and report results for VOTE consensus. They are the compute layer — the more workers with diverse data, the stronger the consensus.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role worker` (default) |
| Color | Green/Blue (`#3fb950` / `#58a6ff`) |
| Executes programs | Yes |
| Signs programs | No |
| Votes on data | No (submits only) |
| Embeds Nebula | No (uses sentient's) |

## Responsibilities

### 1. Program Execution

When a signed program arrives (via UDP, HTTP, or relay), the worker:

1. Verifies the signature (VRFY)
2. Loads its local data into the program's named memory slots
3. Runs TNET training on local data
4. Performs forward-pass inference
5. Stores the result (score, decision, memory state)
6. Forwards the program to peers

Different workers see different data — that's the point. A US worker trains on US transactions, an EU worker on European ones. VOTE consensus combines diverse perspectives.

### 2. Data Submission

Workers observe data in the real world and submit it to the collective:

```bash
curl -X POST http://sentient:9001/data/submit \
  -H "Content-Type: application/json" \
  -d '{
    "name": "transactions_us_east",
    "content": "shape=6,500 data=0.12,0.45,...",
    "author": "worker_us",
    "context": {
      "description": "US East region transactions, March 2026",
      "domain": "fraud_detection",
      "features": ["amount", "frequency", "location_risk", "time_delta", "merchant_category", "card_age"],
      "tags": ["us_east", "march"]
    }
  }'
```

Submitted data enters quarantine and must be approved by sentients (and optionally the oracle) before entering the data pool.

### 3. Result Reporting

Workers expose their execution results via `GET /results`. Other agents collect these during VOTE consensus.

## Endpoints

Workers use the standard collective endpoints — no worker-specific endpoints:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/results` | GET | Execution results for all programs |
| `/health` | GET | Agent status |
| `/peers` | GET | Peer list |

## Dashboard View

The worker dashboard emphasizes execution and data:

- **My Results** — latest execution score, hash, exit code, success/failure
- **Consensus bar** — VOTE result when requested
- **Submit Data** — form with name, content, description, domain, tags, features
- **Oracle section** — Ask panel (if oracle is in the collective)
- **Bottom bar** — Broadcast Program, VOTE Consensus

## Starting a Worker

```bash
# Default role is worker
python3 serve/nml_collective.py --name worker_us --port 9002 \
    --seeds http://localhost:9001 --data demos/agent2.nml.data

# Workers need local data (--data) to produce meaningful scores
```

## Data Context

Workers should include rich context metadata with submissions:

| Field | Purpose | Example |
|-------|---------|---------|
| `description` | Human-readable summary | "US East transactions March 2026" |
| `domain` | Problem domain | "fraud_detection" |
| `features` | Feature names matching shape | ["amount", "frequency", ...] |
| `tags` | Searchable labels | ["us_east", "march", "batch_47"] |
| `source` | Where the data came from | "worker_us" |
| `time_range` | Time period covered | "2026-03-01 to 2026-03-15" |

Context is stored alongside the binary data, indexed in SQLite, and encoded into vector embeddings for semantic search. The Oracle uses context to assess data quality during her auto-vote.
