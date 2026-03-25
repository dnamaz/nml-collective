# NML Collective — End to End

## The Eight Roles

```mermaid
graph TB
    EXT(["External World"])

    EM["Emissary\nboundary"]
    H["Herald\nMQTT broker"]

    O["Oracle\nbrain"]
    S["Sentient\nauthority"]
    A["Architect\nbuilder"]
    C["Custodian\ndata engineer"]
    W1["Worker\ncompute"]
    W2["Worker\ncompute"]
    EN["Enforcer\nimmune system"]

    LLM(["LLM"])

    EXT <-->|REST| EM
    EM <-->|MQTT| H
    H <-->|MQTT| O
    H <-->|MQTT| S
    H <-->|MQTT| A
    H <-->|MQTT| C
    H <-->|MQTT| W1
    H <-->|MQTT| W2
    H <-->|MQTT| EN
    O -.->|reasoning| LLM
    A -.->|code gen| LLM
```

| Role | One-line | Thinks? |
|------|----------|---------|
| **Oracle** | The brain — intent, reasoning, data specs, assessment, vote initiation | Yes |
| **Sentient** | Authority — signs programs, approves data, trust anchor | No — acts on Oracle recommendation |
| **Worker** | Compute — executes programs on local data, reports scores | No — runs what it's told |
| **Architect** | Builder — generates NML programs from Oracle specs | No — builds what the spec says |
| **Custodian** | Data engineer — transforms raw data into tensors, manages Nebula lifecycle | No — shapes data to Oracle spec |
| **Enforcer** | Immune system — identity, rate limits, quarantine | No — applies rules mechanically |
| **Herald** | Infrastructure — MQTT broker, credentials, ACLs | No — routes what's published |
| **Emissary** | Boundary — receives requests, delivers answers, federation | No — passes in, delivers out |

One brain. Seven pairs of hands.

---

## Standing Orders

The Oracle does not need to be consulted on every decision. It issues **standing orders** — pre-authorized policies that other roles can act on without waiting for the Oracle.

```mermaid
flowchart LR
    O["Oracle"]
    O -->|standing order| S["Sentient\nauto-sign programs\nfrom Architect\nwith dry-run pass"]
    O -->|standing order| C["Custodian\nauto-approve data\n< 1MB with valid schema"]
    O -->|standing order| EN["Enforcer\nauto-quarantine\non 5+ violations"]
    O -->|standing order| W["Workers\nauto-commit when\nvariance < 0.01\nacross 3+ workers"]
```

Standing orders handle the 90% case. The Oracle only gets involved for exceptions — novel requests, outlier scores, unknown data domains, ambiguous intent.

---

## End to End: "Who Will Win March Madness?"

### Phase 1 — Request Intake

```mermaid
sequenceDiagram
    participant Human
    participant EM as Emissary
    participant H as Herald
    participant O as Oracle

    Human->>EM: "Who will win the March Madness men's tournament?"
    EM->>EM: Authenticate request
    EM->>H: PUBLISH nml/request/inbound
    note right of EM: raw text, no interpretation
    EM-->>Human: "Request received, processing..."
    H-->>O: DELIVER nml/request/inbound
```

The Emissary does not interpret. It authenticates, acknowledges, and passes the raw request to the mesh. The Oracle picks it up.

### Phase 2 — Oracle Decomposes Intent

```mermaid
sequenceDiagram
    participant O as Oracle
    participant LLM as LLM

    O->>LLM: Decompose: "Who will win March Madness?"
    LLM-->>O: Intent analysis

    note over O: Intent: prediction/classification
    note over O: Domain: sports / NCAA basketball
    note over O: Data needed: team stats, historical outcomes, bracket
    note over O: Features: seed, win_pct, sos, ppg, opp_ppg, ...
    note over O: Architecture: 68 teams × 12 features → probability
    note over O: Output: team with highest P(win)

    O->>O: Produce data spec + program spec
```

The Oracle is the only role that reasons about *what to do*. It produces two outputs:

**Data spec** (for Custodian):
```
domain: "ncaa_basketball_2026"
sources: ["team_season_stats", "historical_tournament", "current_bracket"]
shape: [68, 12]
dtype: float32
features: [seed, win_pct, sos, ppg, opp_ppg, rpg, apg, topg, ftpct, fg3pct, sos_rank, rpi]
```

**Program spec** (for Architect):
```
intent: "predict tournament winner"
architecture: "12→16→8→1"
training: "supervised, historical outcomes"
inference: "rank 68 teams by P(win)"
output_key: "win_probability"
```

### Phase 3 — Data Acquisition

```mermaid
sequenceDiagram
    participant O as Oracle
    participant H as Herald
    participant EM as Emissary
    participant C as Custodian
    participant S as Sentient

    O->>H: PUBLISH nml/data/spec (data requirements)
    H-->>C: DELIVER nml/data/spec
    H-->>EM: DELIVER nml/data/spec

    note over EM: Oracle needs NCAA data
    note over EM: Emissary knows how to reach external APIs

    O->>H: PUBLISH nml/data/fetch (fetch directive to Emissary)
    H-->>EM: DELIVER nml/data/fetch
    EM->>EM: GET https://api.sports-data.io/ncaa/teams/2026
    EM->>EM: GET https://api.sports-data.io/ncaa/tournament/history
    EM->>EM: GET https://api.sports-data.io/ncaa/bracket/2026

    EM->>H: PUBLISH nml/data/raw (raw JSON responses)
    H-->>C: DELIVER nml/data/raw

    note over C: Custodian receives raw data + Oracle's spec
    C->>C: Parse JSON → normalize → reshape to [68, 12] float32
    C->>C: Build tensor: NML\x02 binary object
    C->>C: Build historical outcome tensor for training labels
    C->>C: Initialize weight matrices for 12→16→8→1 architecture

    C->>H: PUBLISH nml/data/submit (tensor objects, quarantine)
    H-->>S: DELIVER nml/data/submit
    H-->>O: DELIVER nml/data/submit

    note over O: Validate: does data match spec?
    note over O: 68 teams × 12 features, all present, no NaN
    O->>H: PUBLISH nml/data/vote approve
    S->>H: PUBLISH nml/data/vote approve (standing order: Oracle approved → auto-approve)

    H-->>C: DELIVER nml/data/approve
    C->>C: Promote from quarantine to Nebula data pool
    C->>H: PUBLISH nml/data/ready (hash refs for each tensor object)
```

### Phase 4 — Program Generation

```mermaid
sequenceDiagram
    participant O as Oracle
    participant H as Herald
    participant A as Architect
    participant LLM as LLM
    participant S as Sentient

    O->>H: PUBLISH nml/spec (program spec)
    H-->>A: DELIVER nml/spec

    A->>LLM: Generate NML: "12→16→8→1 classifier, features=[seed,win_pct,...], output=win_probability"
    LLM-->>A: Symbolic NML program

    A->>A: Dry-run assembly validation
    A->>A: Convert to compact form (pilcrow-delimited)
    A->>A: Verify: output key matches "win_probability"

    A->>H: PUBLISH nml/submit (validated compact NML)
    H-->>S: DELIVER nml/submit

    S->>S: Verify Architect identity
    S->>S: Sign with Ed25519
    S->>H: PUBLISH nml/program (signed, QoS 1)

    note over S: Standing order: programs from Architect<br/>with dry-run pass → auto-sign
```

### Phase 5 — Execution

```mermaid
sequenceDiagram
    participant H as Herald
    participant W1 as Worker 1
    participant W2 as Worker 2
    participant W3 as Worker 3
    participant C as Custodian
    participant EN as Enforcer

    H-->>W1: DELIVER nml/program
    H-->>W2: DELIVER nml/program
    H-->>W3: DELIVER nml/program

    W1->>W1: Verify Ed25519 signature
    W2->>W2: Verify Ed25519 signature
    W3->>W3: Verify Ed25519 signature

    note over EN: Enforcer monitors
    EN->>EN: All workers verified, no quarantine flags

    W1->>C: GET /objects/{hash} (team stats tensor)
    W2->>C: GET /objects/{hash} (team stats tensor)
    W3->>C: GET /objects/{hash} (team stats tensor)

    note over W1: Each worker may have different<br/>supplementary local data<br/>(regional scouting, injury reports)

    W1->>W1: TNET train on historical outcomes
    W1->>W1: Forward pass on 2026 data → scores

    W2->>W2: TNET train on historical outcomes
    W2->>W2: Forward pass on 2026 data → scores

    W3->>W3: TNET train on historical outcomes
    W3->>W3: Forward pass on 2026 data → scores

    W1->>H: PUBLISH nml/result/phash win_probability=0.847 (team=Houston)
    W2->>H: PUBLISH nml/result/phash win_probability=0.823 (team=Houston)
    W3->>H: PUBLISH nml/result/phash win_probability=0.831 (team=Duke)
```

### Phase 6 — Vote and Consensus

```mermaid
sequenceDiagram
    participant O as Oracle
    participant H as Herald
    participant S as Sentient
    participant A as Architect
    participant EN as Enforcer
    participant HER as Herald
    participant EM as Emissary
    participant C as Custodian

    note over O: Oracle collects all results
    O->>O: 3/3 workers reported — quorum met

    note over O: Phase 1 — Statistical assessment
    O->>O: Worker 1: Houston 0.847
    O->>O: Worker 2: Houston 0.823
    O->>O: Worker 3: Duke 0.831
    O->>O: z-scores normal, no outliers
    O->>O: Confidence: high (low variance)

    note over O: Phase 2 — Multi-role vote
    O->>H: PUBLISH nml/vote/initiate

    note over O: Oracle vote
    O->>H: nml/vote/phash — statistical: z-scores clean,<br/>confidence=high, weight=1.2

    note over S: Sentient vote
    S->>H: nml/vote/phash — program properly signed,<br/>data provenance verified, authority weight=1.5

    note over A: Architect vote
    A->>H: nml/vote/phash — scores in expected range<br/>for 12→16→8→1 architecture, weight=1.0

    note over EN: Enforcer vote
    EN->>H: nml/vote/phash — all submitters identity-verified,<br/>no quarantine flags, no rate violations, weight=1.0

    note over HER: Herald vote
    HER->>H: nml/vote/phash — 3/3 workers received program,<br/>100% delivery, mesh healthy, weight=0.8

    note over C: Custodian vote
    C->>H: nml/vote/phash — data tensors intact,<br/>correct shape, no corruption, weight=0.9

    note over EM: Emissary vote
    EM->>H: nml/vote/phash — no conflicting external data,<br/>first request of this type, weight=0.7

    note over S: Sentient combines weighted votes → COMMIT
    S->>S: Weighted consensus: Houston, P(win)=0.834
    S->>H: PUBLISH nml/committed/phash
```

### Phase 7 — Result Delivery

```mermaid
sequenceDiagram
    participant S as Sentient
    participant H as Herald
    participant O as Oracle
    participant C as Custodian
    participant EM as Emissary
    participant Human

    H-->>O: DELIVER nml/committed/phash
    H-->>C: DELIVER nml/committed/phash
    H-->>EM: DELIVER nml/committed/phash

    note over O: Oracle composes the answer
    O->>O: Translate result into natural language
    O->>H: PUBLISH nml/response/outbound

    H-->>EM: DELIVER nml/response/outbound

    note over C: Custodian persists
    C->>C: Store consensus result in Nebula
    C->>C: Link to program + data + votes

    note over EM: Emissary delivers
    EM-->>Human: "Based on analysis of 68 NCAA teams across<br/>12 statistical features, the collective predicts<br/>Houston wins the 2026 tournament with 83.4%<br/>confidence. 3 workers executed the model —<br/>2 predicted Houston, 1 predicted Duke.<br/>Consensus confidence: high."
```

---

## The Complete Pipeline — Summary

```mermaid
flowchart TD
    subgraph intake [1. Intake]
        H1["Human asks question"]
        E1["Emissary receives + authenticates"]
    end

    subgraph think [2. Think]
        O1["Oracle decomposes intent"]
        O2["Oracle produces data spec + program spec"]
    end

    subgraph acquire [3. Acquire]
        EM1["Emissary fetches external data"]
        C1["Custodian transforms → tensors"]
        C2["Custodian submits to quarantine"]
        OS["Oracle + Sentient approve data"]
        C3["Custodian promotes to Nebula"]
    end

    subgraph build [4. Build]
        A1["Architect generates NML from spec"]
        A2["Architect validates (dry-run)"]
        S1["Sentient signs program"]
    end

    subgraph execute [5. Execute]
        H2["Herald distributes to workers"]
        W1["Workers fetch data from Nebula"]
        W2["Workers execute + score"]
    end

    subgraph consensus [6. Consensus]
        O3["Oracle initiates vote"]
        ALL["All 8 roles vote\neach with own criteria"]
        S2["Sentient commits weighted result"]
    end

    subgraph deliver [7. Deliver]
        O4["Oracle composes answer"]
        C4["Custodian persists to Nebula"]
        E2["Emissary delivers to human"]
    end

    intake --> think --> acquire --> build --> execute --> consensus --> deliver
```

---

## Oracle Scaling

The Oracle is the single reasoning layer but not a single point of failure.

```mermaid
flowchart TB
    subgraph fast [Fast Path — No LLM]
        f1["Collect scores"]
        f2["Compute z-scores"]
        f3["Check quorum"]
        f4["Evaluate standing orders"]
        f5["Route known patterns"]
    end

    subgraph slow [Slow Path — LLM]
        s1["Decompose novel intent"]
        s2["Generate data spec"]
        s3["Generate program spec"]
        s4["Compose natural language"]
        s5["Handle ambiguity"]
    end

    subgraph scale [Horizontal Scaling]
        O1["Oracle 1"]
        O2["Oracle 2"]
        O3["Oracle 3"]
        H["Herald\nshared subscription\nload balances across Oracles"]
    end

    H --> O1
    H --> O2
    H --> O3
```

| Path | Needs LLM | Latency | Frequency |
|------|-----------|---------|-----------|
| Score collection + z-score | No | Microseconds | Every result |
| Quorum check | No | Microseconds | Every result |
| Standing order evaluation | No | Milliseconds | Every data/program submission |
| Known intent pattern | No | Milliseconds | Repeated queries |
| Novel intent decomposition | Yes | Seconds | First time per domain |
| Natural language response | Yes | Seconds | Per external request |

90% of Oracle work is the fast path. The LLM only fires for genuinely novel situations.

---

## Failure Modes

| What fails | Impact | Recovery |
|-----------|--------|----------|
| One Worker | Fewer scores, still reaches quorum | Other workers continue |
| One Oracle | Others pick up load (shared subscription) | Scale back up |
| All Oracles | No new reasoning, standing orders still work | Restart Oracle |
| Sentient | No new signatures, no commits | Existing programs keep running |
| Architect | No new programs | Existing programs keep running |
| Custodian | No new data, workers use cached tensors | Restart Custodian |
| Enforcer | No quarantine enforcement, mesh runs open | Restart Enforcer |
| Herald | All MQTT traffic stops | Restart broker (auto-supervised) |
| Emissary | No external access, internal mesh continues | Restart Emissary |
| LLM | Oracle fast path works, slow path queues | Queue until LLM returns |
