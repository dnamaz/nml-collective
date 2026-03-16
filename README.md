# NML Collective — Autonomous Agent Mesh

A decentralized agent collective for [NML](https://github.com/dnamaz/nml) programs. Specialized agents self-discover, broadcast signed programs via UDP multicast, train locally, and reach consensus — no central orchestrator.

## What It Does

- **Four agent roles**: Sentient (authority), Worker (compute), Oracle (knowledge), Architect (builder)
- **Zero-config discovery**: UDP multicast, mDNS/Bonjour, WebSocket relay, HTTP seeds
- **Signed program distribution**: Ed25519-signed NML programs in a single UDP packet (340 bytes symbolic)
- **Local training**: each worker runs TNET on its own data, producing diverse perspectives
- **Two-phase VOTE consensus**: raw scores + Oracle assessment (outlier detection, confidence, weights)
- **Data quarantine**: multi-role voting with Oracle analysis before data enters the pool
- **Program pipeline**: Oracle specs → Architect builds symbolic NML → Sentient signs → Workers execute
- **Real-time dashboard**: role-specific UI with 3D visualization, click-to-focus, zoom

## Quick Start

Requires [NML](https://github.com/dnamaz/nml) runtime built with crypto support:

```bash
cd ../nml && make nml-crypto
```

Start a minimal collective:

```bash
# Sentient (signs programs, manages data)
python3 serve/nml_collective.py --name prime --port 9001 --role sentient --data demos/agent1.nml.data

# Workers (execute programs on regional data)
python3 serve/nml_collective.py --name worker_us --port 9002 --seeds http://localhost:9001 --data demos/agent2.nml.data
python3 serve/nml_collective.py --name worker_eu --port 9003 --seeds http://localhost:9001 --data demos/agent3.nml.data

# Oracle (observes everything, answers questions, votes on data)
python3 serve/nml_collective.py --name sibyl --port 9004 --seeds http://localhost:9001 --role oracle

# Architect (generates NML programs — requires NML LLM)
python3 serve/nml_collective.py --name daedalus --port 9005 --seeds http://localhost:9001 --role architect --llm http://localhost:8082
```

Submit a program, get consensus:

```bash
curl -X POST http://localhost:9001/submit \
  -H "Content-Type: application/json" \
  -d "{\"program\": \"$(cat demos/fraud_detection.nml)\"}"

curl -X POST http://localhost:9001/consensus \
  -H "Content-Type: application/json" -d '{"strategy":"median"}'
```

Open the dashboard: http://localhost:9001/dashboard

## Architecture

```
Oracle ──spec──► Architect ──symbolic──► Sentient ──broadcast──► Workers
  │                                        │                       │
  │ observes all                           │ signs + nebula         │ execute + VOTE
  │◄──── events ◄────── gossip mesh ◄──────┼───────────────────────┤
  │                                        │                       │
  └──── assessment ────► consensus ◄───────┴───── scores ◄─────────┘
```

No single point of failure. Kill any agent and the rest keep running.

## Agent Roles

| Role | Purpose | Executes | Signs | Votes on Data | LLM |
|------|---------|----------|-------|---------------|-----|
| [**Sentient**](docs/ROLE_SENTIENT.md) | Authority — signs programs, approves data, embeds Nebula | Yes | Yes | Yes (authority) | No |
| [**Worker**](docs/ROLE_WORKER.md) | Compute — executes programs, submits data with context | Yes | No | No | No |
| [**Oracle**](docs/ROLE_ORACLE.md) | Knowledge — observes all, answers questions, assesses consensus, votes on data | No | No | Yes (analysis) | Optional |
| [**Architect**](docs/ROLE_ARCHITECT.md) | Builder — generates symbolic NML from specs, validates, ships compact | Dry-run | No | No | Required |

See the individual role documents in `docs/` for full specifications.

## Oracle

The Oracle maintains awareness of every agent, tracks all events, reads the Nebula, and answers questions:

```bash
# Ask a question (works without LLM for structured queries)
curl -X POST http://localhost:9004/ask \
  -H "Content-Type: application/json" \
  -d '{"question": "What scores did everyone get?"}'

# Full collective context
curl http://localhost:9004/context

# Recommendations
curl http://localhost:9004/recommend

# Generate a program spec for the Architect
curl -X POST http://localhost:9004/spec \
  -H "Content-Type: application/json" \
  -d '{"intent": "Detect fraud in European transactions"}'
```

Add `--llm` for open-ended reasoning. Without it, she handles counts, status, scores, agents, events, nebula, consensus, and specific agent lookups.

## Architect

The Architect generates NML programs in symbolic syntax for minimal packet size:

```bash
# Build a program from a spec
curl -X POST http://localhost:9005/build \
  -H "Content-Type: application/json" \
  -d '{"intent": "fraud detection", "features": 6, "architecture": "6→8→1"}'

# Validate an existing program
curl -X POST http://localhost:9005/validate \
  -H "Content-Type: application/json" \
  -d '{"program": "↓ κ @w1\n↓ λ @b1\n◼"}'

# View built programs
curl http://localhost:9005/catalog
```

Requires `--llm` pointing to the NML-trained model. Validates by dry-run assembly with `nml-crypto`.

## Nebula (Persistent Storage)

Sentient agents embed a nebula — a three-layer persistent store:

| Layer | What | Where |
|-------|------|-------|
| Truth | Binary tensors + per-agent transaction chains | `.nebula/objects/` + `.nebula/agents/` |
| Speed | SQLite indexes (status, author, domain, tags) | `.nebula/index.db` |
| Intelligence | Vector embeddings (stats + context semantics) | `.nebula/vectors/` |

Data submissions carry rich context metadata (description, domain, features, tags) stored alongside binary objects. See [docs/NEBULA_DESIGN.md](docs/NEBULA_DESIGN.md) for the full design.

## Two-Phase VOTE

1. **Phase 1** — Raw scores collected from executing agents (workers + sentients)
2. **Phase 2** — Oracle assessment: outlier detection (z-score), confidence (high/medium/low), per-agent weights, weighted consensus

```bash
curl -X POST http://localhost:9001/consensus \
  -H "Content-Type: application/json" -d '{"strategy":"median"}'
# Returns: raw_consensus, consensus (weighted), assessment, weights
```

## Discovery

| Method | Scope | Config |
|--------|-------|--------|
| UDP multicast | Same subnet | `239.78.77.76:7776`, zero-config |
| mDNS/Bonjour | LAN | `_nml._tcp.local.`, zero-config |
| WebSocket relay | WAN | `--relay ws://host:7777/ws` |
| HTTP seeds | Any | `--seeds http://host:port` |

## Demos

```bash
# 3 agents + fraud detection + consensus
bash demos/collective_demo.sh

# Sentient + workers + oracle + Q&A
bash demos/oracle_demo.sh

# Full pipeline: Oracle → Architect → Sentient → Workers → VOTE
bash demos/architect_demo.sh --llm=http://localhost:8082

# Sign + distribute + train + vote + patch
bash demos/distributed_fraud.sh
```

## Endpoints

**All agents:**

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/health` | GET | Agent status |
| `/peers` | GET | Current peer list |
| `/peer/join` | POST | Accept new peer (includes role) |
| `/broadcast` | POST | Receive a program |
| `/submit` | POST | Submit a program (broadcasts to all) |
| `/results` | GET | Execution results |
| `/consensus` | POST | Two-phase VOTE across fleet |
| `/state` | GET | Full agent state |
| `/ws` | GET | WebSocket real-time updates |
| `/dashboard` | GET | Role-specific web dashboard |
| `/discover` | GET | All known agent URLs |
| `/data/submit` | POST | Submit data to quarantine (with context) |
| `/data/approve` | POST | Approve quarantined data (sentient/oracle) |
| `/data/reject` | POST | Reject quarantined data (sentient/oracle) |

**Oracle only:**

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/ask` | POST | Natural language question |
| `/context` | GET | Full collective awareness |
| `/explain` | GET | Explain a hash (program/data/consensus) |
| `/recommend` | GET | Recommendations |
| `/assess` | POST | Assess consensus scores |
| `/spec` | POST | Generate program spec for Architect |

**Architect only:**

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/build` | POST | Build NML from spec |
| `/validate` | POST | Validate NML program |
| `/catalog` | GET | Built programs |

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

## Dependencies

- [NML](https://github.com/dnamaz/nml) runtime (`nml-crypto` binary)
- Python 3.10+
- `aiohttp` (`pip install aiohttp`)
- `zeroconf` (`pip install zeroconf`) — optional, for mDNS discovery
