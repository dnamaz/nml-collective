# Custodian — Data Engineer

## Role

The Custodian is the collective's data engineer. It sits between the outside world and the rest of the mesh: raw data (JSON arrays, CSV) is posted to its `/ingest` endpoint, transformed into NebulaDisk float32 tensors, and submitted to the Sentient for quarantine approval. Once approved, the Custodian serves the binary objects directly to Workers over HTTP.

The Custodian does not execute NML programs, sign anything, or vote on consensus. Its job is to produce clean, addressable, approved tensor objects and keep them available to the rest of the mesh.

It was added to the original seven roles to relieve the Sentient of raw-data handling. The Sentient still owns approval authority; the Custodian owns **transformation, storage, and serving**.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role custodian` |
| Default HTTP port | `9004` |
| Executes programs | No |
| Signs programs | No |
| Votes on data | No (submits to Sentient) |
| Owns storage | Yes — local NebulaDisk object store |

## Responsibilities

### 1. Ingestion and Transformation

Raw data arrives via `POST /ingest`. The Custodian:

1. Parses the payload as JSON float array or CSV rows (`format` field selects the parser)
2. Flattens everything to a single `float32` buffer
3. If the buffer is ≤ `SHARD_FLOAT_COUNT` (8192 floats), stores it as a single NebulaDisk object
4. Otherwise splits into shards of 8192 floats each, stores each shard, and builds a **manifest** JSON object listing the shard hashes

Manifests carry up to `MANIFEST_MAX_SHARDS` (1024) shards, so a single dataset can hold up to ~8M floats.

### 2. Quarantine Submission

After storing the object (or manifest), the Custodian calls `POST /data/submit` on the Sentient with the hash, name, sample count, and author. The Sentient runs its quarantine logic. Shards themselves are never submitted — they are internal storage artifacts with `STATUS_SHARD`, promoted to `STATUS_APPROVED` only when their parent manifest is approved.

### 3. Approval Tracking

The Custodian subscribes to two MQTT topics published by the Sentient:

- `nml/data/approve` — flips the item (and, if a manifest, every listed shard) to `STATUS_APPROVED` and publishes `nml/data/ready` so Workers know the object is available
- `nml/data/reject` — flips the item (and its shards) to `STATUS_REJECTED`; the object stays on disk but is no longer served

### 4. Serving to Workers

Workers fetch data over HTTP from the Custodian:

- `GET /objects/<hash>` — full NebulaDisk `.obj` file (binary)
- `GET /objects/<hash>?offset=N&count=M` — raw `float32` slice of the content payload, for workers that only need part of a sharded dataset

Only approved objects are served; pending or rejected hashes return `403`. Each successful read increments a `served_count` persisted alongside the object.

### 5. Staleness Monitoring

Every `STALE_POLL_S` (60 s), the Custodian scans approved items and publishes `nml/data/stale` for anything older than `--stale-after` (default 3600 s). A per-item cooldown prevents re-alerting more than once per stale interval.

### 6. Persistence and Recovery

Each object has a sidecar `.meta` file (`objects/<aa>/<hash>.meta`) storing fields that aren't in the NebulaDisk binary: name, author, status, timestamps, served_count. On startup the Custodian walks the objects directory and rebuilds its in-memory registry from these sidecars (Unix only; Windows starts empty).

## Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/ingest` | POST | Accept raw JSON/CSV data, transform, submit to Sentient |
| `/objects/<hash>` | GET | Serve full NebulaDisk object (approved only) |
| `/objects/<hash>?offset=&count=` | GET | Serve raw `float32` slice |
| `/objects/<hash>` | DELETE | Remove object and cascade-delete manifest shards |
| `/pool` | GET | List all tracked items (excludes internal shards) |
| `/approve` | POST | Manual approval (admin/testing) |
| `/health` | GET | Agent status and item counts |
| `/peers` | GET | Known mesh peers |

## Topic Responsibilities

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `nml/data/ready` | Publish | Object approved and serveable at `port` |
| `nml/data/stale` | Publish | Object has passed its freshness threshold |
| `nml/data/approve` | Subscribe | Sentient approval notifications |
| `nml/data/reject` | Subscribe | Sentient rejection notifications |
| `nml/heartbeat/<name>` | Publish | Standard mesh heartbeat |

## Data Layout

```
<data-dir>/
└── objects/
    ├── a3/
    │   ├── a3f1...b9.obj       # NebulaDisk binary (full object or manifest)
    │   └── a3f1...b9.meta      # sidecar: name, author, status, served_count
    └── ...
```

- Objects are content-addressed by a 16-char hex hash
- The first two hex chars are used as a shard directory to keep any single folder small
- Manifests are regular NebulaDisk objects whose content begins with `{"type":"manifest"...` — the Custodian detects them with a short probe read

## Relationship with Sentient

The Sentient owns approval authority; the Custodian owns data handling. A submission flow looks like:

1. External client posts raw data to the Custodian's `/ingest`
2. Custodian stores the object (or shards + manifest) locally
3. Custodian calls `POST /data/submit` on the Sentient with the hash
4. Sentient decides (quarantine, consensus, its own checks) and publishes `nml/data/approve` or `nml/data/reject`
5. Custodian flips status, publishes `nml/data/ready`, and begins serving Workers

The Custodian never signs, never votes, never rewrites the Sentient's decision.

## Relationship with Workers

Workers receive only the hash and the Custodian's `host:port` (carried in `nml/data/ready`). They fetch the object over HTTP, load the `float32` slice into their local memory, and execute programs against it. The range-fetch mode (`?offset=&count=`) lets a Worker pull only the portion of a sharded dataset it needs.

## Starting a Custodian

```bash
./custodian_agent \
    --name custodian_us \
    --broker 127.0.0.1 --broker-port 1883 \
    --port 9004 \
    --data-dir ./.custodian-data \
    --sentient 127.0.0.1 --sentient-port 9001 \
    --stale-after 3600
```

## Implementation Notes (C99)

- Registry is a fixed-size array of 256 items; when full, the first rejected slot is evicted
- Max single-request body is 4 MiB; max dataset is `MANIFEST_MAX_SHARDS × SHARD_FLOAT_COUNT` floats (~8M)
- HTTP server is single-threaded with a 10 s client timeout so a slow client cannot stall the MQTT loop
- Manifest detection is done by probing the first 18 bytes of the stored object for `{"type":"manifest"`
- Shard storage bypasses quarantine by design — only the manifest is an approvable unit

## Gaps / TODO

- [ ] No `ROLE_CUSTODIAN.md` had existed before this document; the role was undocumented
- [ ] Sentient submission is HTTP-only; other agents mostly use MQTT — consider aligning
- [ ] No introspection endpoint for shard structure (`GET /manifests/<hash>`)
- [ ] No authentication on `/ingest` or `/approve`
- [ ] Registry restore is Unix-only (Windows restarts empty and relies on re-ingestion)
- [ ] `/approve` admin endpoint bypasses Sentient entirely — useful for testing, risky in production
