# NML Collective — Autonomous Agent Mesh

A decentralized agent collective for [NML](https://github.com/dnamaz/nml) programs. Specialized agents self-discover, broadcast signed programs via MQTT, train locally, and reach consensus — no central orchestrator.

## What It Does

- **Seven agent roles**: Sentient (authority), Worker (compute), Oracle (knowledge), Architect (builder), Enforcer (immune system), Herald (mesh infrastructure), Emissary (external boundary)
- **MQTT transport**: publish/subscribe mesh via Herald broker — QoS delivery, retained presence, Last Will on disconnect
- **Signed program distribution**: Ed25519-signed NML programs in a single packet (340 bytes symbolic)
- **Local training**: each worker runs TNET on its own data, producing diverse perspectives
- **Two-phase VOTE consensus**: raw scores + Oracle assessment (outlier detection, confidence, weights)
- **Data quarantine**: multi-role voting with Oracle analysis before data enters the pool
- **Program pipeline**: Oracle specs → Architect builds symbolic NML → Sentient signs → Workers execute
- **External boundary**: all human and inter-collective traffic enters through the Emissary

## Quick Start

Build the C99 collective (requires GCC and [NML](https://github.com/dnamaz/nml) runtime):

```bash
make          # builds edge/libcollective.a + all 8 role binaries
```

Start a minimal collective:

```bash
# Herald (MQTT broker — must start first)
./roles/herald/herald_agent --broker-port 1883 &

# Sentient (signs programs, manages data)
./roles/sentient/sentient_agent --name prime --port 9001 \
  --broker 127.0.0.1 --data demos/agent1.nml.data &

# Workers (execute programs on regional data)
./roles/worker/worker_agent --name worker_us \
  --broker 127.0.0.1 --data demos/agent2.nml.data &
./roles/worker/worker_agent --name worker_eu \
  --broker 127.0.0.1 --data demos/agent3.nml.data &

# Oracle (z-score consensus, per-agent confidence weights)
./roles/oracle/oracle_agent --name sibyl --port 9010 --broker 127.0.0.1 &

# Architect (generates NML programs from specs via template engine)
./roles/architect/architect_agent --name daedalus --port 9005 --broker 127.0.0.1 &

# Enforcer (identity checks, rate limits, quarantine gossip)
./roles/enforcer/enforcer_agent --name guardian --broker 127.0.0.1 &

# Emissary (external REST API + webhook delivery)
./roles/emissary/emissary_agent --name gateway --port 8080 \
  --broker 127.0.0.1 --api-key secret &
```

Submit a program and check consensus:

```bash
# Via Sentient (signs + broadcasts to all workers)
curl -X POST http://localhost:9001/program \
  -H "Content-Type: text/plain" \
  --data-binary @demos/fraud_detection.nml

# Oracle assessment (weighted mean, per-agent confidence)
curl http://localhost:9010/assessments
curl http://localhost:9010/agents
```

Or use the all-in-one demo:

```bash
bash demos/collective_demo.sh
```

## Architecture

```
                        ┌─────────────────┐
  humans, other         │    Emissary      │  external boundary
  collectives, APIs ───►│  REST · gateway  │◄─── inter-collective
                        └────────┬────────┘
                                 │
                        ┌────────▼────────┐
                        │     Herald       │  MQTT broker
                        │  credentials     │  all internal traffic
                        │  ACLs · $SYS     │  flows through here
                        └────────┬────────┘
           ┌─────────────────────┼─────────────────────┐
           │                     │                     │
    ┌──────▼──────┐      ┌───────▼──────┐      ┌──────▼──────┐
    │   Sentient  │      │    Worker    │      │   Oracle    │
    │  data+sign  │      │   execute    │      │  consensus  │
    └─────────────┘      └─────────────┘      └─────────────┘
           │                                          │
    ┌──────▼──────┐                          ┌────────▼────┐
    │  Architect  │                          │  Enforcer   │
    │   builder   │                          │  immune sys │
    └─────────────┘                          └─────────────┘
```

No single point of failure among the agent roles. Kill any agent and the rest keep running. Herald can be clustered for high availability.

## Agent Roles

| Role | Purpose | Signs | Executes | Votes on Data |
|------|---------|-------|----------|---------------|
| [**Sentient**](docs/ROLE_SENTIENT.md) | Authority — signs programs, approves data, embeds Nebula | Yes | Yes | Yes (authority) |
| [**Worker**](docs/ROLE_WORKER.md) | Compute — executes programs, submits data, reports results | No | Yes | No |
| [**Oracle**](docs/ROLE_ORACLE.md) | Knowledge — observes all, answers questions, assesses consensus | No | No | Yes (analysis) |
| [**Architect**](docs/ROLE_ARCHITECT.md) | Builder — generates symbolic NML from specs, validates, ships compact | Dry-run | No | No |
| [**Enforcer**](docs/ROLE_ENFORCER.md) | Immune system — quarantines nodes, bans, evidence, gossips enforcement | No | No | No |
| [**Herald**](docs/ROLE_HERALD.md) | Mesh infrastructure — MQTT broker, credentials, ACLs, health | No | No | No |
| [**Emissary**](docs/ROLE_EMISSARY.md) | External boundary — human API, inter-collective federation, ingestion, export | No | No | No |

See the individual role documents in `docs/` for full specifications.

## Oracle

The Oracle collects result votes from all workers, runs z-score outlier detection per program, and maintains per-agent confidence weights via EWMA:

```bash
# All program assessments (weighted mean, raw mean, confidence, outlier count)
curl http://localhost:9010/assessments

# Assessment for a specific program hash
curl http://localhost:9010/assessments/<phash>

# Per-agent confidence weights and vote history
curl http://localhost:9010/agents

# Health check
curl http://localhost:9010/health
```

## Architect

The Architect subscribes to `nml/spec` and generates NML programs via a built-in template engine. An optional LLM HTTP backend (`--llm-host`) is used when no template matches:

```bash
# Available templates
curl http://localhost:9005/templates

# Generate a program from an intent
curl -X POST http://localhost:9005/generate \
  -H "Content-Type: application/json" \
  -d '{"intent": "fraud detection on credit card transactions", "features": 6}'

# View catalog of generated programs
curl http://localhost:9005/catalog
```

## Enforcer

The Enforcer is a MQTT daemon — no HTTP API. It monitors all mesh traffic, verifies Ed25519 node identities, enforces rate limits (20 msg/60s), and detects z-score outliers (threshold 2.5). Enforcement decisions are broadcast as `MSG_ENFORCE` on `nml/enforce`:

- `W` = warning (logged, auto-expires 1h)
- `Q` = quarantine (temporary isolation, gossiped to all peers)
- `U` = unquarantine
- `B` = blacklist (permanent, requires sentient approval via `--approve AGENT`)

Enforcement effects are visible via Oracle's `/agents` — quarantined agents show reduced confidence.

## Herald

The Herald supervises the Mosquitto broker and manages credentials:

```bash
# Health / broker metrics
curl http://localhost:9000/health

# Issue credentials for a new agent
curl -X POST http://localhost:9000/credentials \
  -H "Content-Type: application/json" \
  -d '{"agent": "worker_new", "role": "worker"}'

# Revoke credentials
curl -X DELETE http://localhost:9000/credentials/worker_bad
```

## Emissary

The Emissary is the external boundary — Bearer token auth, per-IP rate limiting (10 RPS), webhook delivery:

```bash
# Submit a program from outside the collective
curl -X POST http://localhost:8080/spec \
  -H "Authorization: Bearer secret" \
  -H "Content-Type: application/json" \
  -d '{"intent": "fraud detection", "features": 6}'

# Get assessment results
curl -H "Authorization: Bearer secret" http://localhost:8080/results

# Register a webhook (fires on new assessments or enforce events)
curl -X POST http://localhost:8080/webhooks \
  -H "Authorization: Bearer secret" \
  -H "Content-Type: application/json" \
  -d '{"url": "https://my-system.example/hook", "events": ["result", "enforce"]}'
```

## Nebula (Persistent Storage)

Sentient agents embed a nebula — a three-layer persistent store:

| Layer | What | Where |
|-------|------|-------|
| Truth | Binary tensors + per-agent transaction chains | `.nebula/objects/` + `.nebula/agents/` |
| Speed | SQLite indexes (status, author, domain, tags) | `.nebula/index.db` |
| Intelligence | Vector embeddings (stats + context semantics) | `.nebula/vectors/` |

Data submissions carry rich context metadata (description, domain, features, tags) stored alongside binary objects. See [docs/NEBULA_DESIGN.md](docs/NEBULA_DESIGN.md) for the full design.

## Two-Phase VOTE

1. **Phase 1** — Workers execute and publish `MSG_RESULT` scores to `nml/result/<phash>`
2. **Phase 2** — Oracle runs z-score outlier detection, updates per-agent EWMA confidence weights, publishes weighted consensus to `nml/assess/<phash>`

```bash
# Oracle weighted assessment
curl http://localhost:9010/assessments/<phash>
# Returns: raw_mean, weighted_mean, confidence, vote_count, outlier_count
```

## Transport

| Method | Scope | Role |
|--------|-------|------|
| MQTT | LAN + WAN | Herald broker, all agent-to-agent traffic |
| MQTT retained | LAN + WAN | Presence announcements — new joiners see current peers immediately |
| MQTT Last Will | LAN + WAN | Automatic departure announcements on ungraceful disconnect |
| REST/HTTP | External | Emissary only — human and inter-collective interface |

## C99 Implementation

All eight roles are implemented in C99 and run on Linux x86_64, ARM Cortex-M4, and other embedded targets. Built from `edge/libcollective.a` (shared library) and `roles/` (one binary per role):

```bash
make                         # libcollective.a + all 8 role binaries
make -C roles/worker         # single role
make -C edge arm             # cross-compile for ARM Cortex-M4
```

Binary sizes on ARM Cortex-M4 (with MQTT): ~52KB flash, ~20KB RAM for worker.

See [plans/C99_EDGE_WORKER.md](plans/C99_EDGE_WORKER.md) for platform targets and implementation details.

## Demos

```bash
# Full collective — all 8 roles, complete pipeline
bash demos/demo.sh
bash demos/demo.sh --llm-host=http://localhost:8082   # with LLM for Architect

# nml-crypto standalone — sign + distribute + train + VOTE + patch (no broker)
bash demos/distributed_fraud.sh

# Start a single agent manually
bash demos/agent.sh worker   --name w_1    --data demos/agent1.nml.data
bash demos/agent.sh oracle   --name sibyl  --port 9010
bash demos/agent.sh enforcer --name guard
```

## Documentation

| Document | Content |
|----------|---------|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Protocols, transport, consensus, pipeline |
| [SYSTEM_ARCHITECTURE.md](docs/SYSTEM_ARCHITECTURE.md) | Full 7-layer stack, data flow, metrics |
| [NEBULA_DESIGN.md](docs/NEBULA_DESIGN.md) | Storage design, quarantine, data lifecycle |
| [ROLE_SENTIENT.md](docs/ROLE_SENTIENT.md) | Sentient role specification |
| [ROLE_WORKER.md](docs/ROLE_WORKER.md) | Worker role specification |
| [ROLE_ORACLE.md](docs/ROLE_ORACLE.md) | Oracle role specification |
| [ROLE_ARCHITECT.md](docs/ROLE_ARCHITECT.md) | Architect role specification |
| [ROLE_ENFORCER.md](docs/ROLE_ENFORCER.md) | Enforcer role specification |
| [ROLE_HERALD.md](docs/ROLE_HERALD.md) | Herald role specification |
| [ROLE_EMISSARY.md](docs/ROLE_EMISSARY.md) | Emissary role specification |

## Dependencies

- [NML](https://github.com/dnamaz/nml) runtime (C source — included via `nml_exec.c`)
- Mosquitto (MQTT broker — managed by Herald)
- GCC (C99) or `arm-none-eabi-gcc` for ARM cross-compilation
- `nml-crypto` binary — only for `demos/distributed_fraud.sh` (signing tool)
