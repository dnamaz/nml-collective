# C99 Edge Worker Plan

**Status:** Draft  
**Created:** 2026-03-15  
**Goal:** Define a C99 implementation path for NML collective agents on micro devices.

---

## 1. Context

The current `serve/` layer is Python (aiohttp, asyncio, subprocess). It runs well on desktop, server, and Raspberry Pi. For micro devices (MCUs, embedded Linux with tight RAM), we need a portable, performant alternative.

**Key insight:** The NML runtime (`nml-crypto`) is already C. We can link it directly instead of spawning subprocesses.

---

## 2. Scope

### In scope
- Minimal **edge worker** agent in C99
- UDP multicast discovery + program receive
- In-process NML execution (linked library)
- Result reporting (UDP or minimal HTTP POST)
- Ed25519 signature verification (tweetnacl)

### Out of scope (for C99 edge)
- Full Nebula ledger, SQLite, vector embeddings
- Oracle, Architect, Enforcer roles
- WebSocket relay, mDNS
- Dashboard serving
- LLM integration

---

## 3. Target Platforms

| Platform        | RAM    | Flash  | Notes                    |
|----------------|--------|--------|--------------------------|
| ARM Cortex-M4+ | 64KB+  | 256KB+ | Bare metal, lwIP or raw  |
| ESP32          | 320KB  | 4MB    | FreeRTOS, WiFi, good fit |
| RISC-V (SiFive)| 64KB+  | 256KB+ | Bare metal               |
| Linux SBC      | 32MB+  | —      | Mongoose, full TCP stack |

---

## 4. Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    C99 Edge Worker                           │
├─────────────────────────────────────────────────────────────┤
│  UDP Layer        │  Parse NML\x01 messages                  │
│  (multicast rx)   │  MSG_ANNOUNCE, MSG_PROGRAM, MSG_HEARTBEAT│
├─────────────────────────────────────────────────────────────┤
│  Crypto           │  tweetnacl (Ed25519 verify)              │
│  (from nml repo)  │  Signature check on program              │
├─────────────────────────────────────────────────────────────┤
│  NML Runtime      │  nml.c + nml_crypto.h (linked)           │
│  (from nml repo)  │  Execute in-process, no subprocess        │
├─────────────────────────────────────────────────────────────┤
│  Report           │  UDP multicast MSG_RESULT                │
│                   │  OR HTTP POST to sentient URL             │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. Message Protocol (UDP)

Reuse existing wire format from `nml_collective.py`:

```
4 bytes:  magic "NML\x01"
1 byte:   msg_type (1=ANNOUNCE, 2=PROGRAM, 3=RESULT, 4=HEARTBEAT)
1 byte:   name_len  (max 63; agent_name is truncated to 63 bytes on encode)
N bytes:  agent_name (UTF-8)
2 bytes:  http_port (big-endian)
rest:     payload (UTF-8 for program/result)
```

**MSG_PROGRAM payload — compact format:**

Programs are compacted before transmission using a pilcrow (`\xb6`, U+00B6) as a line delimiter:

```
compact = join("\xb6", [line.strip() for line in program.splitlines()
                         if line.strip() and not line.strip().startswith(";")])
```

`msg.c` must implement both directions:
- `msg_program_to_compact()` — strip comments, trim whitespace, join with `\xb6`
- `msg_compact_to_program()` — split on `\xb6`, join with `\n`

**MSG_RESULT payload format:** `{program_hash_16}:{score:.6f}` (e.g. `a1b2c3d4e5f6a7b8:0.823141`)

**Self-filter:** Discard any received packet whose `agent_name` matches the local agent name. Without this, the agent processes its own multicast reflections.

---

## 6. Dependencies

| Dependency | Purpose              | Source / License   |
|------------|----------------------|--------------------|
| NML runtime| Execute programs     | `../nml/runtime/`  |
| tweetnacl | Ed25519 verify       | `../nml/runtime/`  |
| lwIP      | UDP (bare metal)     | Optional, BSD      |
| Mongoose  | UDP + HTTP (Linux)   | Optional, GPL/CE   |
| cJSON     | JSON for HTTP body   | Optional, MIT      |

> **Crypto open question:** The Python sentient signs programs by calling `nml-crypto --sign --key <hex>` where `--key` is documented as an HMAC key, not an Ed25519 key. Verify whether the NML runtime's signing scheme is HMAC-SHA256 or Ed25519 before implementing `crypto.c`. The dependency on tweetnacl should be replaced or removed accordingly.

---

## 7. Project Layout (Proposed)

```
nml-collective/
├── serve/                    # Python (unchanged)
├── plans/
│   └── C99_EDGE_WORKER.md    # This plan
└── edge/                     # New C99 tree
    ├── Makefile
    ├── config.h              # Platform config (RAM, features)
    ├── main.c                # Entry, event loop
    ├── udp.c / udp.h         # Multicast send/recv
    ├── msg.c / msg.h         # Parse/encode NML messages
    ├── crypto.c / crypto.h   # Verify wrapper (tweetnacl)
    ├── nml_exec.c / nml_exec.h  # NML run wrapper
    ├── report.c / report.h  # Send result (UDP/HTTP)
    └── nml/                  # Submodule or copy of ../nml/runtime
        ├── nml.c
        ├── nml_crypto.h
        └── tweetnacl.c
```

---

## 8. Build Strategy

1. **Phase 1 — Standalone test:** Build `edge/` with Mongoose on Linux. No NML link yet; stub execute. Verify UDP rx/tx, compact decode (pilcrow split), and self-filter.
2. **Phase 2 — NML link:** Add NML runtime as static lib. Implement `nml_exec_run(program, data)` → score. Parse combined stdout+stderr for `=== MEMORY ===` block; extract score using the `fraud_score` → `risk_score` → `score` → `result` key priority.
3. **Phase 3 — Crypto:** Confirm signing scheme (HMAC-SHA256 vs Ed25519 — see §6 open question). Implement and wire up `crypto_verify_program`. Reject unsigned or invalid programs.
4. **Phase 4 — Cross-compile:** Add ARM/RISC-V toolchain. Swap Mongoose for lwIP or raw sockets per platform.

---

## 9. Configuration (config.h)

```c
#define EDGE_AGENT_NAME     "edge_1"
#define UDP_MULTICAST_GROUP "239.78.77.76"
#define UDP_MULTICAST_PORT  7776
#define HTTP_REPORT_URL     NULL   /* or "http://192.168.1.10:9001/result" */
#define NML_MAX_CYCLES      1000000
/* Max compact program bytes in a single UDP payload. Python allows up to 65000
   bytes (UDP_MAX_PACKET) but typical programs compact to 500-700 bytes.
   Raise this if larger programs are used; keep under available RAM on MCU. */
#define NML_MAX_PROGRAM_LEN 4096
#define USE_HTTP_REPORT      0     /* 1 = POST to sentient, 0 = UDP only */
```

---

## 10. API Surface (Minimal)

```c
/* msg.h */
/* name_sz must be >= 64 (agent names are capped at 63 bytes + NUL on encode) */
int msg_parse(const uint8_t *buf, size_t len, int *type, char *name, size_t name_sz, uint16_t *port, char *payload, size_t payload_sz);
int msg_encode_result(uint8_t *buf, size_t buf_sz, const char *name, uint16_t port, const char *program_hash, const char *score);

/* Compact ↔ program conversion (pilcrow \xb6 delimiter, strips ; comments) */
int msg_program_to_compact(const char *program, char *out, size_t out_sz);
int msg_compact_to_program(const char *compact, char *out, size_t out_sz);

/* crypto.h */
int crypto_verify_program(const char *program, size_t len);

/* nml_exec.h */
/* Runs program in-process. Parses combined stdout+stderr for the MEMORY dump
   (between "=== MEMORY ===" and "=== STATS ===" markers). Extracts the first
   matching key from: fraud_score, risk_score, score, result — in that order.
   Returns 0 on success with score_out filled; -1 on exec failure or no score. */
int nml_exec_run(const char *program, const char *data, char *score_out, size_t score_sz);

/* report.h */
int report_send_udp(const char *program_hash, const char *score);
int report_send_http(const char *url, const char *program_hash, const char *score);
```

---

## 11. Estimated Size

| Component   | Flash | RAM   |
|-------------|-------|-------|
| NML runtime | ~20KB | ~4KB  |
| tweetnacl   | ~8KB  | ~1KB  |
| UDP + msg   | ~4KB  | ~2KB  |
| App logic   | ~6KB  | ~2KB  |
| **Total**   | ~40KB | ~10KB |

With lwIP: add ~30KB flash, ~8KB RAM. With Mongoose: add ~50KB flash, ~12KB RAM.

---

## 12. Risks & Mitigations

| Risk                    | Mitigation                                      |
|-------------------------|-------------------------------------------------|
| NML runtime not linkable| Expose `nml_run()` C API in nml repo            |
| Platform-specific UDP   | Abstract `udp_send` / `udp_recv` per platform   |
| JSON for HTTP           | Use fixed-format POST or minimal cJSON          |
| Memory fragmentation    | Static buffers, no malloc in hot path           |

---

## 13. Success Criteria

- [ ] Edge worker joins multicast, receives programs from Python sentient
- [ ] Correctly decodes compact pilcrow-delimited programs (strips `;` comments, splits on `\xb6`)
- [ ] Discards own multicast reflections (self-filter by agent name)
- [ ] Executes NML in-process, parses `=== MEMORY ===` from combined stdout+stderr, extracts score using `fraud_score` → `risk_score` → `score` → `result` key priority
- [ ] Reports result via UDP (`{hash16}:{score:.6f}`) or HTTP POST to sentient
- [ ] Verifies program signature, rejects invalid (scheme confirmed against NML binary)
- [ ] Builds for Linux (x86_64) and at least one MCU (e.g. ESP32)
- [ ] Binary under 80KB flash, 20KB RAM on MCU

---

## 14. References

- `serve/nml_collective.py` — UDP encoding, message types, multicast config
- `docs/SYSTEM_ARCHITECTURE.md` — Wire format, NML signing
- [dnamaz/nml](https://github.com/dnamaz/nml) — NML runtime, nml_crypto.h
