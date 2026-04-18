# Role: Worker

The muscle of the collective. Workers execute signed programs against data fetched from the Custodian and report results for VOTE consensus. They are the compute layer — the more workers with diverse local state, the stronger the consensus.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role worker` (default) |
| Color | Green (`#3fb950`) |
| Executes programs | Yes |
| Signs programs | No |
| Ingests data | No (see Custodian) |
| Votes on data | No |
| Embeds Nebula | No (fetches approved objects from Custodian) |

## Responsibilities

### 1. Program Execution

When a signed program arrives (via MQTT or the UDP fallback), the worker:

1. Verifies the signature (VRFY)
2. Fetches any referenced datasets from the Custodian (`GET /objects/<hash>`), caching locally
3. Loads data into the program's named memory slots
4. Runs TNET training on local data
5. Performs forward-pass inference
6. Publishes the score via `MSG_RESULT` for VOTE consensus
7. Retains a short history of recent scores (see `GET /results`)

Different workers run the same program against different cached datasets — that's the point. A US-region worker operating on US data and an EU-region worker on EU data will produce different scores; VOTE consensus combines their diverse perspectives.

### 2. Result Reporting

Workers expose their recent execution history via HTTP:

- `GET /results` — newest-first JSON array of `{phash, score, ts}`
- `GET /data/cache` — currently cached datasets (name, hash, size, status)
- `GET /health`, `GET /peers` — standard

The Oracle polls these endpoints to build its cross-agent assessment.

## What Workers Do NOT Do

- **Ingest raw data.** All data ingestion goes through the [Custodian](ROLE_CUSTODIAN.md), which normalizes JSON/CSV into FLOAT32 NebulaDisk objects, shards large datasets, submits them to the Sentient for quarantine approval, and serves approved objects over HTTP. Workers fetch from the Custodian; they never accept external observations directly.
- **Sign programs.** Only the Sentient holds the signing key.
- **Vote on data quality.** The Sentient approves or rejects; the Oracle assesses advisorily.

An embedded Worker running on observation hardware (e.g. a sensor) should still forward its observations to a nearby Custodian rather than submitting to the mesh directly — this keeps the ingestion surface narrow and well-tested. If a deployment genuinely needs direct worker ingestion (offline MCU, no Custodian reachable), that capability can be added back as an optional mode.

## Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Role-tailored landing page (embedded HTML) |
| `/health` | GET | Agent status: name, peers, results recorded, cache size |
| `/results` | GET | Most recent 32 execution results |
| `/data/cache` | GET | Current local dataset cache |
| `/peers` | GET | Known mesh peers |

## Dashboard View

The Worker landing page (served at `/` by the Worker binary itself) emphasizes execution and data flow:

- **Recent Execution Results** — last 32 runs with program hash, score, age
- **Local Data Cache** — names, hashes, sizes, cache status (ready / loading / rejected)
- **Peers** — compact list
- **Footer** — direct links to `/health`, `/results`, `/data/cache`, `/peers`

Auto-refreshes every 3 seconds.

## Starting a Worker

```bash
./roles/worker/worker_agent \
    --name worker_us \
    --port 9002 \
    --broker 127.0.0.1 --broker-port 1883 \
    --data demos/agent2.nml.data   # optional: preload a local dataset
```

If `--data` is omitted, the worker starts with an empty cache and fetches objects on demand from the Custodian when the first program references them.

## Design Principle

The Worker's role is compute, not observation. This separation lets the Custodian stay the single source of truth for ingestion (one code path for sharding, approval, and serving), keeps the Worker binary small enough for embedded targets, and ensures the approval lifecycle can't be bypassed by a rogue observer.
