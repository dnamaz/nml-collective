# NML Collective — Architecture

## Overview

The NML Collective is a decentralized agent mesh where autonomous peers discover each other, broadcast signed NML programs, execute them locally, and reach consensus — with no central orchestrator.

```mermaid
flowchart TB
    subgraph collective [Agent Collective]
        S["Sentient\n(authority)"]
        W1["Worker 1\n(compute)"]
        W2["Worker 2\n(compute)"]
        O["Oracle\n(knowledge)"]
        A["Architect\n(builder)"]
    end
    subgraph external [External Services]
        nml_llm["NML LLM\nnml_server.py :8082"]
        relay_svc["WAN Relay\nnml_relay.py :7777"]
    end
    S <-->|gossip| W1
    S <-->|gossip| W2
    S <-->|gossip| O
    S <-->|gossip| A
    W1 <-->|gossip| W2
    O -.->|observe all| S
    O -.->|observe all| W1
    O -.->|observe all| W2
    O -.->|spec| A
    A -.->|generate| nml_llm
    S <-.->|WAN| relay_svc
```

**Key principle:** Every agent is a peer with a specialized role. There is no leader, no hub, no single point of failure. Kill any agent and the rest continue operating.

### Four Roles

| Role | Purpose | Signs | Executes | Votes on Data | Generates Programs |
|------|---------|-------|----------|---------------|-------------------|
| [**Sentient**](ROLE_SENTIENT.md) | Authority — signs programs, approves data, embeds Nebula | Yes | Yes | Yes | No |
| [**Worker**](ROLE_WORKER.md) | Compute — executes programs, submits data, reports results | No | Yes | No | No |
| [**Oracle**](ROLE_ORACLE.md) | Knowledge — observes all, answers questions, assesses consensus, votes on data quality | No | No | Yes (analysis) | Specs only |
| [**Architect**](ROLE_ARCHITECT.md) | Builder — generates NML from specs via NML LLM, validates, ships symbolic | No | Dry-run | No | Yes |

See the individual role documents for full details.

---

## Discovery Protocol

Agents use four discovery layers simultaneously. The first one that succeeds adds the peer.

```mermaid
flowchart LR
    subgraph auto [Automatic — Zero Config]
        mdns["mDNS/Bonjour\n_nml._tcp.local.\nLAN-wide"]
        udp["UDP Multicast\n239.78.77.76:7776\nSame subnet"]
    end
    subgraph configured [Configured]
        relay_disc["WebSocket Relay\n--relay ws://host:7777/ws\nCross-network"]
        seeds_disc["HTTP Seeds\n--seeds http://host:port\nManual fallback"]
    end
    mdns -->|ServiceInfo| validate
    udp -->|ANNOUNCE msg| validate
    relay_disc -->|register JSON| validate
    seeds_disc -->|GET /peers| validate
    validate["Heartbeat\nvalidation"] -->|pass| add_peer["Add to\npeer list"]
    validate -->|fail| discard["Discard\nstale entry"]
```

### mDNS/Bonjour

- Registers `{name}._nml._tcp.local.` with `host_ttl=10, other_ttl=10` (10-second expiry)
- Uses `AsyncZeroconf` + `AsyncServiceBrowser` for non-blocking discovery
- On discovery: resolves address, then **heartbeat-validates** before trusting
- On shutdown: sends mDNS goodbye via `atexit` + `SIGTERM`/`SIGINT` handlers

### UDP Multicast

- Multicast group: `239.78.77.76:7776`, TTL=2
- ANNOUNCE messages broadcast every 5 seconds
- Also carries full NML programs (compact form, single packet)
- Latency: **11 µs median** (vs 127 µs HTTP)

### WebSocket Relay (WAN)

- Agent connects outbound to relay (NAT-friendly)
- Relay forwards all messages to all connected agents
- Same message format as UDP (binary) or JSON (structured)
- Reconnects with 5-second backoff

### HTTP Seeds

- `GET /peers` returns the full peer list as JSON
- `POST /peer/join` announces self to seed
- Fallback for networks where multicast and mDNS don't reach

---

## Broadcast Protocol

When an agent receives a program (via `/submit`, `/broadcast`, or UDP), it follows this flow:

```mermaid
flowchart TD
    receive["Receive program\n(HTTP, UDP, or relay)"]
    dedup{"Already seen?\n(hash check)"}
    receive --> dedup
    dedup -->|yes| drop[Drop duplicate]
    dedup -->|no| execute["Execute locally\n(nml-crypto + local data)"]
    execute --> store["Store result\n(hash → score)"]
    store --> forward_udp{"UDP available?"}
    forward_udp -->|yes| udp_send["UDP multicast\n(1 packet, 340 bytes)"]
    forward_udp -->|no| http_forward["HTTP POST /broadcast\nto each peer"]
    store --> forward_relay{"Relay connected?"}
    forward_relay -->|yes| relay_send["WebSocket send\n(JSON or binary)"]
    forward_relay -->|no| skip_relay[Skip]
    store --> notify["Push to /ws clients\n(WebSocket dashboard)"]
```

### Deduplication

Programs are identified by `SHA-256(program)[:16]` (first 16 hex chars). Once a hash is seen, the program is never re-executed or re-forwarded.

### Epidemic Broadcast

Every agent forwards to all peers except the source. With N agents, the program reaches all nodes in O(log N) hops (epidemic spreading). UDP multicast reaches the entire subnet in one hop.

---

## UDP Packet Format

NML programs are compact enough to fit in a single UDP packet.

```
┌─────────┬──────┬──────────┬────────┬──────────┬──────────────────────┐
│ Magic   │ Type │ Name Len │  Name  │  Port    │  Payload             │
│ 4 bytes │ 1 B  │  1 B     │  N B   │  2 B     │  variable            │
│ NML\x01 │      │          │        │ big-end  │                      │
└─────────┴──────┴──────────┴────────┴──────────┴──────────────────────┘
```

| Message Type | Code | Payload |
|-------------|------|---------|
| ANNOUNCE | 1 | (empty) |
| PROGRAM | 2 | Compact NML (pilcrow-delimited) |
| RESULT | 3 | `{hash}:{score}` |
| HEARTBEAT | 4 | (empty) |

### Size Example

| | Classic | Symbolic Compact | UDP Packet |
|---|---|---|---|
| Fraud detection (23 instr) | 1,985 B | 340 B | 384 B |
| Fits in 1 UDP packet? | No | **Yes** | **Yes** |

---

## Two-Phase Consensus (VOTE)

Any agent can initiate consensus. If an Oracle is in the mesh, the result includes assessment and weights.

```mermaid
sequenceDiagram
    participant Req as Requester
    participant W1 as Worker 1
    participant W2 as Worker 2
    participant O as Oracle

    Note over Req: Phase 1 — Collect raw scores
    Req->>W1: GET /results?program=X
    W1-->>Req: {score: 0.7362}
    Req->>W2: GET /results?program=X
    W2-->>Req: {score: 0.7354}
    Req->>Req: raw median = 0.7354

    Note over O: Phase 2 — Oracle assessment
    Req->>O: POST /assess {scores}
    O-->>Req: {confidence: high, weights, outliers}
    Req->>Req: weighted median = 0.7354
```

**Phase 1:** Raw scores collected from executing agents (workers + sentients).
**Phase 2:** Oracle contributes outlier detection, confidence scoring, per-agent weights, and weighted consensus.

Strategies: `median` (default), `mean`, `min`, `max`.

## Program Pipeline

The full lifecycle from intent to execution:

```mermaid
sequenceDiagram
    participant O as Oracle
    participant A as Architect
    participant S as Sentient
    participant W1 as Worker 1
    participant W2 as Worker 2

    O->>O: Analyze collective needs
    O->>A: POST /build (spec)
    A->>A: NML LLM → symbolic
    A->>A: Validate (dry-run assembly)
    A->>S: POST /submit (symbolic NML)
    S->>S: Sign (Ed25519)
    S->>W1: Broadcast (UDP/HTTP)
    S->>W2: Broadcast (UDP/HTTP)
    W1->>W1: Execute + score
    W2->>W2: Execute + score
    S->>S: VOTE consensus
    O->>O: Assess result
```

---

## WebSocket Dashboard

Every agent serves a real-time dashboard at `/dashboard`.

```mermaid
flowchart LR
    browser["Browser"] -->|"ws://agent:port/ws"| ws_endpoint["/ws endpoint"]
    ws_endpoint -->|snapshot on connect| browser
    ws_endpoint -->|push on every event| browser
    browser -->|auto-discover peers| other_ws["Other agents' /ws"]
```

- **On connect:** sends full state snapshot (peers, programs, results, events)
- **On event:** pushes incremental update (peer join/leave, program broadcast, execution result)
- **Multi-agent:** dashboard opens WebSocket to each discovered agent
- **Reconnect:** 3-second backoff on disconnect
- **Auto-connect:** `/dashboard` injects the agent's own URL, no manual entry needed

---

## Signing and Verification

Programs are signed before distribution. The NML runtime verifies before execution.

```mermaid
flowchart LR
    subgraph signer [Signer]
        keygen["nml-crypto --keygen\n→ private:public"]
        sign["nml-crypto --sign\nprogram.nml --key private"]
    end
    subgraph header [SIGN Header]
        h["SIGN agent=authority\nkey=ed25519:PUBLIC_KEY\nsig=SIGNATURE"]
    end
    subgraph verifier [Any Agent]
        vrfy["VRFY: verify sig\nwith PUBLIC_KEY\n→ pass or TRAP"]
    end
    keygen --> sign
    sign --> header
    header --> vrfy
```

- **Ed25519:** Private key stays local. Only public key in the header. 64-byte signature.
- **HMAC-SHA256:** Backward-compatible for trusted networks. Shared key in header.
- **Tamper detection:** Any modification to the program body invalidates the signature.

---

## File Structure

```
serve/
  nml_collective.py    — Autonomous gossip agent (main entry point, all roles)
  nml_oracle.py        — Oracle knowledge engine (awareness, Q&A, specs, data voting)
  nml_architect.py     — Architect program builder (NML LLM, validation, symbolic)
  nml_nebula.py        — Nebula ledger (quarantine, approval, data pool, consensus)
  nml_storage.py       — Three-layer storage (disk, SQLite, vectors)
  nml_relay.py         — WebSocket relay for WAN
  nml_agent.py         — Hub-and-spoke agent (legacy)

dashboard/
  nml_collective_dashboard.html  — Role-aware single-file web UI

demos/
  collective_demo.sh             — 3 agents + fraud detection + consensus
  oracle_demo.sh                 — Sentient + workers + oracle + Q&A
  architect_demo.sh              — Full pipeline: oracle → architect → sentient → workers
  distributed_fraud.sh           — Sign + distribute + train + vote + patch
  nebula_demo.sh                 — Data quarantine + approval + pool
  fraud_detection.nml            — Example program (classic syntax)
  fraud_detection_symbolic.nml   — Same program (symbolic, 340 bytes)
  agent{1,2,3}.nml.data          — Regional agent data

docs/
  ARCHITECTURE.md                — This document (protocols, transport, storage)
  SYSTEM_ARCHITECTURE.md         — Full 7-layer stack, data flow, metrics
  NEBULA_DESIGN.md               — Storage design, quarantine, data lifecycle
  ROLE_SENTIENT.md               — Sentient role specification
  ROLE_WORKER.md                 — Worker role specification
  ROLE_ORACLE.md                 — Oracle role specification
  ROLE_ARCHITECT.md              — Architect role specification
```

---

## Nebula Storage

The nebula persists all data across three layers. Only Layer 1 is truth — Layers 2 and 3 are derived and rebuildable.

```mermaid
flowchart TD
    submit["Worker submits data"]
    submit --> binary["Write binary tensor\n.nebula/objects/"]
    submit --> tx["Append to agent chain\n.nebula/agents/"]
    binary --> sqlite["Index in SQLite\n.nebula/index.db"]
    binary --> vec["Compute embedding\n.nebula/vectors/"]
    tx --> sqlite
    sqlite --> query["Query: status, author\ntimestamp, @name"]
    vec --> similar["Semantic: find similar\nfind compatible"]
```

| Layer | Purpose | File | Rebuildable? |
|-------|---------|------|-------------|
| 1a: Objects | Binary tensors by hash | `.nebula/objects/` | No (source of truth) |
| 1b: Chains | Per-agent transaction log | `.nebula/agents/` | No (source of truth) |
| 2: Index | Fast queries | `.nebula/index.db` | Yes (from Layer 1) |
| 3: Vectors | Semantic search | `.nebula/vectors/` | Yes (from Layer 1) |

### Transaction Chain Integrity

Each agent's chain is hash-linked. Cross-agent references create a DAG:

```mermaid
flowchart LR
    subgraph oracle_chain [oracle]
        O0["tx#0 JOIN"] --> O1["tx#1 PUBLISH"] --> O2["tx#2 APPROVE"] --> O3["tx#3 CONSENSUS"]
    end
    subgraph w1_chain [worker_1]
        W0["tx#0 JOIN"] --> W1["tx#1 SUBMIT"] --> W2["tx#2 EXECUTION"]
    end
    W1 -.->|ref| O2
    W2 -.->|ref| O3
```

Verify chain integrity: `GET /ledger/verify?agent=oracle`

---

## Dependencies

| Dependency | Purpose | Required? |
|-----------|---------|-----------|
| [NML runtime](https://github.com/dnamaz/nml) | `nml-crypto` binary for execution + signing | Yes |
| Python 3.10+ | Agent runtime | Yes |
| aiohttp | HTTP server + WebSocket | Yes |
| zeroconf | mDNS/Bonjour discovery | Optional |
