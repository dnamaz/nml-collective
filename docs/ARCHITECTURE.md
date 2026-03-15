# NML Collective — Architecture

## Overview

The NML Collective is a decentralized agent mesh where autonomous peers discover each other, broadcast signed NML programs, execute them locally, and reach consensus — with no central orchestrator.

```mermaid
flowchart TB
    subgraph collective [Agent Collective]
        A1["Agent 1\n:9001"]
        A2["Agent 2\n:9002"]
        A3["Agent 3\n:9003"]
        AN["Agent N\n:900N"]
    end
    subgraph external [External Services]
        llm["Central LLM\nnml_server.py :8082"]
        relay_svc["WAN Relay\nnml_relay.py :7777"]
    end
    A1 <-->|gossip| A2
    A2 <-->|gossip| A3
    A3 <-->|gossip| AN
    A1 <-->|gossip| A3
    A1 -.->|generate| llm
    A2 -.->|generate| llm
    A1 <-.->|WAN| relay_svc
    AN <-.->|WAN| relay_svc
```

**Key principle:** Every agent is identical. There is no leader, no hub, no single point of failure. Kill any agent and the rest continue operating.

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

## Consensus (VOTE)

Any agent can initiate consensus:

```mermaid
sequenceDiagram
    participant Requester as Agent 2
    participant A1 as Agent 1
    participant A3 as Agent 3

    Requester->>Requester: Local result for hash X
    Requester->>A1: GET /results?program=X
    A1-->>Requester: {score: 0.7362}
    Requester->>A3: GET /results?program=X
    A3-->>Requester: {score: 0.7354}
    Requester->>Requester: VOTE median(0.7226, 0.7362, 0.7354) = 0.7354
```

Strategies: `median` (default), `mean`, `min`, `max`.

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
  nml_collective.py    — Autonomous gossip agent (main entry point)
  nml_relay.py         — WebSocket relay for WAN
  nml_agent.py         — Hub-and-spoke agent (legacy, backward compat)

dashboard/
  nml_collective_dashboard.html  — Single-file web UI (HTML+JS+CSS)

demos/
  collective_demo.sh             — Start 3 agents + dashboard
  distributed_fraud.sh           — Sign + distribute + train + vote + patch
  fraud_detection.nml            — Example program
  fraud_detection_symbolic.nml   — Same program, 340 bytes compact
  fraud_detection.nml.data       — Training + test data
  agent{1,2,3}.nml.data          — Regional agent data

docs/
  ARCHITECTURE.md                — This document
```

---

## Dependencies

| Dependency | Purpose | Required? |
|-----------|---------|-----------|
| [NML runtime](https://github.com/dnamaz/nml) | `nml-crypto` binary for execution + signing | Yes |
| Python 3.10+ | Agent runtime | Yes |
| aiohttp | HTTP server + WebSocket | Yes |
| zeroconf | mDNS/Bonjour discovery | Optional |
