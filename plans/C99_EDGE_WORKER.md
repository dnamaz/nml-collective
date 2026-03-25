# C99 Collective — Implementation Plan

**Status:** In Progress
**Created:** 2026-03-15
**Updated:** 2026-03-23
**Goal:** Full C99 implementation of all seven collective roles for embedded and server targets.

---

## 1. Context

The current `serve/` layer is Python (aiohttp, asyncio, subprocess). It runs well on desktop, server, and Raspberry Pi. For micro devices (MCUs, embedded Linux with tight RAM), we need a portable, performant alternative.

**Key insight:** The NML runtime (`nml-crypto`) is already C. We can link it directly instead of spawning subprocesses.

The C99 implementation lives in `edge/` (shared library `libcollective.a`) and `roles/` (one binary per role). All roles link against `libcollective.a`.

---

## 2. Transport

The C99 collective uses **MQTT** via the Herald broker. The Herald manages Mosquitto/EMQX; agents are MQTT clients.

For embedded targets (ARM Cortex-M4) without a full TCP stack, the edge library retains a UDP multicast fallback. The UDP wire format is preserved for interoperability with the Python layer during transition.

| Transport | Use | Status |
|-----------|-----|--------|
| MQTT (TCP) | Primary — server and embedded Linux | Done — `mqtt_transport.c` via MQTT-C |
| UDP multicast | Fallback — bare metal MCU, transition | Done |
| HTTP (TCP) | Data fetch — worker pulls objects from sentient | Done |

---

## 3. Current State

### Implemented (`edge/libcollective.a`)

| Module | File | Status |
|--------|------|--------|
| UDP multicast | `udp.c / udp.h` | Done — sender IP capture via recvfrom |
| Wire format | `msg.c / msg.h` | Done — all 5 message types incl. MSG_ENFORCE |
| Ed25519 verify | `crypto.c / crypto.h` | Done |
| Node identity | `identity.c / identity.h` | Done — verify_payload added |
| Peer registry | `peer_table.c / peer_table.h` | Done — IP + role fields |
| Vote aggregation | `vote.c / vote.h` | Done |
| Result reporting | `report.c / report.h` | Done |
| Program broadcast | `program_send.c` | Done |
| HTTP GET client | `http_client.c / http_client.h` | Done |
| NML execution | `nml_exec.c / nml_exec.h` | Done |
| NebulaDisk storage | `storage.c / storage.h` | Done — content-addressed, binary NML\x02 format |
| MQTT transport | `mqtt_transport.c / mqtt_transport.h` | Done — LWT, subscribe nml/#, ring-buffer queue |
| Template engine | `templates.c / templates.h` | Done — 5 standard ML patterns |
| Wire format | `msg.c / msg.h` | Done — MSG_SPEC (type 6) added |

### Implemented (roles)

| Role | File | Status |
|------|------|--------|
| Worker | `roles/worker/worker_agent.c` | Done — fetch-on-miss, quarantine check, MSG_ENFORCE handler |
| Enforcer | `roles/enforcer/enforcer_agent.c` | Done — identity verify, rate limiting, z-score outlier, quarantine gossip |
| Sentient | `roles/sentient/sentient_agent.c` | Done — MQTT, signing, consensus, HTTP data server |
| Oracle | `roles/oracle/oracle_agent.c` | Done — z-score outlier detection, confidence weights, assessments |
| Architect | `roles/architect/architect_agent.c` | Done — template engine + LLM fallback, dry-run validation |
| Herald | `roles/herald/herald_agent.c` | Done — Mosquitto supervisor, ACL, credential REST API |
| Emissary | `roles/emissary/emissary_agent.c` | Done — external REST API, proxy, webhooks, rate limiting |
| Custodian | `roles/custodian/custodian_agent.c` | Done — data ingestion, tensor storage, quarantine submit, staleness |

---

## 4. Target Platforms

| Platform | RAM | Flash | Notes |
|---------|-----|-------|-------|
| ARM Cortex-M4+ | 64KB+ | 256KB+ | Bare metal, UDP transport |
| ESP32 | 320KB | 4MB | FreeRTOS, WiFi, MQTT or UDP |
| RISC-V (SiFive) | 64KB+ | 256KB+ | Bare metal |
| Linux SBC | 32MB+ | — | Full MQTT + HTTP |
| Linux x86_64 | — | — | Development, CI, server |

---

## 5. Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    libcollective.a                           │
├─────────────────────────────────────────────────────────────┤
│  Transport    │  udp.c (multicast) / mqtt_client.c (planned) │
├─────────────────────────────────────────────────────────────┤
│  Wire format  │  msg.c — encode/decode all 5 message types   │
├─────────────────────────────────────────────────────────────┤
│  Crypto       │  crypto.c — Ed25519 via tweetnacl            │
│               │  identity.c — machine_hash + node_id         │
├─────────────────────────────────────────────────────────────┤
│  Mesh state   │  peer_table.c — peers with IP + role         │
│               │  vote.c — two-phase VOTE aggregation         │
├─────────────────────────────────────────────────────────────┤
│  NML          │  nml_exec.c — in-process execution           │
├─────────────────────────────────────────────────────────────┤
│  Network      │  http_client.c — blocking GET for data fetch │
│               │  report.c — UDP/HTTP result reporting        │
└─────────────────────────────────────────────────────────────┘
```

Each role binary links against `libcollective.a` and adds its own logic.

---

## 6. Remaining Work

### MQTT client (`edge/mqtt_client.c / mqtt_client.h`)

Implement MQTT 3.1.1 client over TCP as a drop-in alongside `udp.c`. Key operations:

- CONNECT / CONNACK (username/password auth, Last Will registration)
- PUBLISH (QoS 0 and QoS 1 with PUBACK)
- SUBSCRIBE / SUBACK
- PINGREQ / PINGRESP (keepalive)
- DISCONNECT

Library option: vendor [MQTT-C](https://github.com/LiamBindle/MQTT-C) (single `.c` + `.h`, MIT, embedded-first) — same pattern as TweetNaCl.

### Sentient (`roles/sentient/sentient_agent.c`)

- HTTP server (TCP accept loop, minimal request parser)
- NebulaDisk storage (`storage.c/h`) — C port of `nml_storage.py` binary format
- Program broadcast to mesh
- Data quarantine + approval workflow
- Credential issuance interface for Herald

### Herald (`roles/herald/herald_agent.c`)

- Generate `mosquitto.conf` from CLI arguments
- Exec into Mosquitto or supervise as child process
- Subscribe to `$SYS/broker/#` for health monitoring
- REST endpoints for credential issuance and revocation

### Oracle (`roles/oracle/oracle_agent.c`)

- Subscribe to all result topics
- Z-score consensus assessment
- Weighted median computation
- Publish assessments to `nml/assess/<phash>`

### Architect (`roles/architect/architect_agent.c`)

- Subscribe to `nml/spec` for program requests
- LLM integration for NML program generation
- Dry-run validation via `nml_exec_run`
- Publish validated programs to `nml/submit`

### Emissary (`roles/emissary/emissary_agent.c`)

- HTTP server (REST API for external interface)
- MQTT client connected to Herald
- Webhook registry and delivery
- Inter-collective federation protocol

### Storage (`edge/storage.c / storage.h`)

C port of the NebulaDisk binary format:

```
NML\x02 | hash(8B) | type(1B) | author_len(1B) | author |
timestamp(8B BE double) | ndims(1B) | shape(ndims*4B) |
dtype(1B) | content_len(4B BE) | content
```

Needed by: Sentient (write), Worker (cache), Emissary (serve).

---

## 7. Build

```bash
# Build everything
make

# Build edge library only
make edge

# Build a specific role
make -C roles/worker
make -C roles/enforcer

# Cross-compile for ARM Cortex-M4
make -C edge arm ARM_CC=arm-none-eabi-gcc ARM_NEWLIB=1

# Clean all
make clean
```

---

## 8. Configuration (`edge/config.h`)

```c
#define EDGE_AGENT_NAME      "edge_1"          /* override with -DEDGE_AGENT_NAME='"name"' */
#define EDGE_HTTP_PORT       9099
#define UDP_MULTICAST_GROUP  "239.78.77.76"    /* legacy / embedded fallback */
#define UDP_MULTICAST_PORT   7776
#define NML_MAX_PROGRAM_LEN  4096
#define NML_MAX_TENSOR_SIZE  1048576           /* 1 MB default */
#define NML_MAX_CYCLES       1000000
#define HEARTBEAT_INTERVAL   5
#define EDGE_MACHINE_ID_STR  "nml_edge_default_uid"
```

---

## 9. Estimated Size

| Component | Flash | RAM |
|-----------|-------|-----|
| NML runtime | ~20KB | ~4KB |
| tweetnacl | ~8KB | ~1KB |
| UDP + msg | ~4KB | ~2KB |
| crypto + identity | ~6KB | ~2KB |
| peer_table + vote | ~4KB | ~6KB |
| http_client | ~2KB | ~1KB |
| App logic (worker) | ~8KB | ~4KB |
| **Total (worker)** | ~52KB | ~20KB |

With MQTT client (MQTT-C): add ~10KB flash, ~4KB RAM.

---

## 10. Success Criteria

- [x] Edge library builds clean for Linux x86_64 (libcollective.a)
- [x] Worker joins mesh, receives programs, executes, reports results
- [x] Worker verifies Ed25519 signatures, rejects invalid programs
- [x] Worker fetches data from sentient on demand (HTTP GET /objects)
- [x] Worker respects quarantine decisions (MSG_ENFORCE)
- [x] Enforcer verifies node identity on every ANNOUNCE/HEARTBEAT
- [x] Enforcer rate-limits and detects score outliers
- [x] Enforcer broadcasts quarantine via MSG_ENFORCE
- [x] ARM Cortex-M4 cross-compile target (`make arm`)
- [ ] MQTT client replacing UDP as primary transport
- [ ] Sentient serves data over HTTP, broadcasts programs
- [ ] Herald supervises Mosquitto and manages credentials
- [ ] Emissary exposes REST API and inter-collective federation
- [ ] Oracle collects results and publishes consensus assessments
- [ ] Binary under 80KB flash, 20KB RAM on ARM Cortex-M4

---

## 11. References

- `serve/nml_collective.py` — UDP encoding, message types, multicast config
- `serve/nml_enforcer.py` — Enforcer behavior reference
- `docs/ARCHITECTURE.md` — Wire format, MQTT topics, consensus protocol
- `docs/ROLE_*.md` — Individual role specifications
- [dnamaz/nml](https://github.com/dnamaz/nml) — NML runtime, nml_crypto.h, tweetnacl
