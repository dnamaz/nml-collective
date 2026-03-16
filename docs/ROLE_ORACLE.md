# Role: Oracle

The knowledge layer of the collective. The Oracle maintains awareness of every agent, tracks all events, reads the Nebula ledger, answers questions via LLM reasoning, votes on data quality with analysis-backed reasoning, generates program specifications for the Architect, and assesses consensus results.

She never signs programs and never executes NML — but she is the most informed participant in the collective.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role oracle` |
| Color | Golden (`#d29922`) |
| Executes programs | No |
| Signs programs | No |
| Votes on data | Yes (knowledge weight, with analysis) |
| Embeds Nebula | Read-only view |
| LLM | Recommended (`--llm`) for deep reasoning; works without for structured queries |

## Responsibilities

### 1. Collective Awareness

The Oracle actively polls `/state` from all peers every 10 seconds and subscribes to `/ws` WebSocket events from every discovered agent. She maintains:

- **Collective state cache** — full state of every agent (role, peers, programs, scores, uptime, data status)
- **Unified event timeline** — all events from all agents, chronologically merged
- **Nebula snapshot** — latest stats from sentient(s)
- **Programs catalog** — all known program hashes and their results

### 2. Question Answering

Any agent (or human via curl) can ask the Oracle questions:

```bash
curl -X POST http://oracle:9004/ask \
  -H "Content-Type: application/json" \
  -d '{"question": "What scores did everyone get?"}'
```

**Without LLM** — handles structured queries by pattern matching: agent counts, status, scores, roles, events, nebula state, consensus, specific agent lookups, and more.

**With LLM** (`--llm`) — reasons about open-ended questions using a dynamic system prompt stuffed with live collective state. Maintains conversation history for follow-ups.

### 3. Data Quality Voting

When data enters quarantine, the Oracle auto-analyzes it:

- **Statistical checks** — variance, boundary value detection (adversarial padding), distribution analysis
- **Context validation** — feature count vs shape match, description/domain presence
- **Author reliability** — connectivity, peer count, uptime
- **Quality score** — 0.0 to 1.0 composite → approve / review / abstain / reject

Her vote carries a `role: "oracle"` tag and includes the full analysis. Sentients see this analysis alongside their own approve/reject buttons.

### 4. Consensus Assessment

During two-phase VOTE, the Oracle contributes:

- **Outlier detection** — z-score analysis, flags agents > 1.5σ from mean
- **Confidence scoring** — high / medium / low based on spread, std, agent count
- **Per-agent weights** — trust weights based on data provenance, connectivity, uptime
- **Weighted consensus** — scores replicated proportionally to weight

### 5. Program Specification

The Oracle reasons about what programs the collective needs:

```bash
curl -X POST http://oracle:9004/spec \
  -H "Content-Type: application/json" \
  -d '{"intent": "Detect fraud in European transactions"}'
```

She produces a structured specification including intent, architecture, features, data bindings, training parameters, and output syntax — ready for the Architect to build.

## Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/ask` | POST | Ask a question (natural language) |
| `/context` | GET | Full collective awareness |
| `/explain` | GET | Explain a program/data/consensus by hash |
| `/recommend` | GET | Recommendations for the collective |
| `/assess` | POST | Assess consensus scores (called by other agents) |
| `/spec` | POST | Generate program specification for Architect |

## Dashboard View

The Oracle dashboard emphasizes knowledge and observability:

- **Oracle panel** — stats (agents tracked, events, programs, LLM status, poll cycles) + Ask input
- **Recommendations** — auto-loaded, severity-colored (warning/action/info)
- **Nebula** — read-only view (no approve/reject buttons)
- **Full event feed** — 60 events vs 30 for other roles
- **Agent cards** — extra detail (data status, transport info)
- **Bottom bar** — hidden (oracle doesn't execute or vote on programs)
- **Canvas** — dashed golden lines to ALL agents, "observing" label, golden glow

## Starting an Oracle

```bash
# Without LLM (structured answers only)
python3 serve/nml_collective.py --name sibyl --port 9004 \
    --seeds http://localhost:9001 --role oracle

# With LLM (deep reasoning)
python3 serve/nml_collective.py --name sibyl --port 9004 \
    --seeds http://localhost:9001 --role oracle --llm http://localhost:8082
```

## Design Principle

The Oracle's authority comes from knowledge, not power. She can't force anything — her vote on data is one voice among many, her assessment is advisory, her specifications are requests. But because she sees everything, her contributions are the most informed in the collective.
