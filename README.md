# NML Collective — Autonomous Agent Mesh

A decentralized agent collective for [NML](https://github.com/dnamaz/nml) programs. Agents self-discover, broadcast signed programs via UDP multicast, train locally, and reach consensus — no central orchestrator.

## What It Does

- **Zero-config discovery**: agents find each other via UDP multicast, mDNS/Bonjour, or WebSocket relay
- **Signed program distribution**: Ed25519-signed NML programs broadcast in a single UDP packet (340 bytes)
- **Local training**: each agent runs TNET on its own data, adapting the model locally
- **VOTE consensus**: agents collect scores and compute median/mean/min/max across the fleet
- **Real-time dashboard**: every agent serves a web UI at `/dashboard` showing the mesh topology
- **WAN relay**: WebSocket relay server for agents across networks

## Quick Start

Requires [NML](https://github.com/dnamaz/nml) runtime built with crypto support:

```bash
# In the NML repo
cd ../nml && make nml-crypto
```

Start a collective (zero config — agents discover each other automatically):

```bash
# Terminal 1
python3 serve/nml_collective.py --name alpha --port 9001 --data demos/agent1.nml.data

# Terminal 2
python3 serve/nml_collective.py --name bravo --port 9002 --data demos/agent2.nml.data

# Terminal 3
python3 serve/nml_collective.py --name charlie --port 9003 --data demos/agent3.nml.data
```

Submit a program (broadcasts to all agents):

```bash
curl -X POST http://localhost:9001/submit \
  -H "Content-Type: application/json" \
  -d "{\"program\": \"$(cat demos/fraud_detection.nml)\"}"
```

Get consensus (two-phase VOTE — raw scores + oracle assessment + weighted result):

```bash
curl -X POST http://localhost:9001/consensus \
  -H "Content-Type: application/json" -d '{"strategy":"median"}'
```

Open the dashboard: http://localhost:9001/dashboard

## Architecture

```
Agent ←→ Agent ←→ Agent       (UDP multicast + gossip)
  ↕         ↕         ↕
 LLM       LLM       LLM      (shared central NML server)
  ↕         ↕         ↕
Dashboard Dashboard Dashboard  (self-hosted on every agent)
```

No single point of failure. Kill any agent and the rest keep running.

### Agent Roles

| Role | Purpose | Executes? | Writes Nebula? |
|------|---------|-----------|----------------|
| **Sentient** | Signs programs, approves data, embeds nebula | No | Yes |
| **Worker** | Submits data, executes programs, reports results | Yes | Quarantine only |
| **Oracle** | Observes all agents, answers questions via LLM, generates specs | No | Votes with analysis |
| **Architect** | Generates NML programs from specs via NML LLM | Dry-run only | No |

## Oracle (Knowledge Layer)

The Oracle observes everything and decides nothing. She tracks every agent in the mesh, aggregates events, reads the Nebula ledger, and connects to an LLM for deep inference reasoning.

```bash
# Start an Oracle (connects to all agents, answers questions)
python3 serve/nml_collective.py --name sibyl --port 9004 \
    --seeds http://localhost:9001 --role oracle --llm http://localhost:8082

# Ask a question
curl -X POST http://localhost:9004/ask \
  -H "Content-Type: application/json" \
  -d '{"question": "Which agents have executed programs and what were their scores?"}'

# Get full collective context
curl http://localhost:9004/context

# Get recommendations
curl http://localhost:9004/recommend

# Explain a specific program or data hash
curl "http://localhost:9004/explain?hash=958c212f"
```

The Oracle works without an LLM (structured data answers), but connects to one via `--llm` for deep reasoning. She never executes programs, never signs anything, never approves data — she only observes and answers.

```bash
# Full demo: sentient + 2 workers + oracle
bash demos/oracle_demo.sh
```

## Nebula (Persistent Storage)

Sentient agents embed a nebula — a three-layer persistent store:

| Layer | What | Where |
|-------|------|-------|
| Truth | Binary tensors + per-agent transaction chains | `.nebula/objects/` + `.nebula/agents/` |
| Speed | SQLite indexes for fast queries | `.nebula/index.db` |
| Intelligence | Vector embeddings for semantic search | `.nebula/vectors/` |

Data survives restarts. Transaction chains are hash-linked and tamper-proof. Vector embeddings enable "find data compatible with this program" queries.

```bash
# Sentient starts with persistent nebula
python3 serve/nml_collective.py --name oracle --port 9001 --role sentient

# Query the ledger
curl http://localhost:9001/nebula/stats
curl http://localhost:9001/data/pool
curl "http://localhost:9001/data/similar?hash=089405d9"
```

See [docs/NEBULA_DESIGN.md](docs/NEBULA_DESIGN.md) for the full storage architecture.

## Discovery Methods

| Method | Scope | Config | How |
|--------|-------|--------|-----|
| UDP multicast | Same subnet | None | `239.78.77.76:7776` announce every 5s |
| mDNS/Bonjour | LAN | None | `_nml._tcp.local.` service registration |
| WebSocket relay | WAN | `--relay ws://host:7777/ws` | NAT-friendly outbound connection |
| Seed list | Any | `--seeds http://host:port` | HTTP gossip fallback |

## WAN Relay

For agents on different networks:

```bash
# Start relay server
python3 serve/nml_relay.py --port 7777

# Agents connect to relay
python3 serve/nml_collective.py --name agent_1 --port 9001 --relay ws://relay:7777/ws
```

## Demos

```bash
# Automated demo: 3 agents + fraud detection + consensus
bash demos/collective_demo.sh

# Distributed fraud detection: sign + distribute + train + vote + patch
bash demos/distributed_fraud.sh

# Oracle demo: sentient + workers + oracle + ask questions
bash demos/oracle_demo.sh

# Architect demo: full pipeline Oracle → Architect → Sentient → Workers
bash demos/architect_demo.sh --llm=http://localhost:8082
```

## Agent Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/health` | GET | Agent status |
| `/peers` | GET | Current peer list |
| `/peer/join` | POST | Accept new peer |
| `/broadcast` | POST | Receive a program |
| `/submit` | POST | Submit a program (broadcasts to all) |
| `/generate` | POST | Generate NML via central LLM |
| `/results` | GET | Execution results |
| `/consensus` | POST | Collect and VOTE across fleet |
| `/state` | GET | Full agent state (for dashboard) |
| `/ws` | GET | WebSocket push (real-time updates) |
| `/dashboard` | GET | Self-hosted web dashboard |
| `/discover` | GET | All known agent URLs |
| `/ask` | POST | Ask the Oracle a question (oracle only) |
| `/context` | GET | Oracle's full collective awareness (oracle only) |
| `/explain` | GET | Explain a program/data hash (oracle only) |
| `/recommend` | GET | Oracle's recommendations (oracle only) |
| `/assess` | POST | Oracle assesses consensus scores (oracle only) |
| `/spec` | POST | Oracle generates program spec (oracle only) |
| `/build` | POST | Architect builds NML from spec (architect only) |
| `/validate` | POST | Architect validates NML program (architect only) |
| `/catalog` | GET | Architect's built programs (architect only) |

## Dependencies

- [NML](https://github.com/dnamaz/nml) runtime (`nml-crypto` binary)
- Python 3.10+
- `aiohttp` (`pip install aiohttp`)
- `zeroconf` (`pip install zeroconf`) — optional, for mDNS discovery
