# Role: Architect

The builder of the collective. The Architect receives structured program specifications (from the Oracle or external requests), generates valid NML programs via the NML LLM, validates them by dry-run assembly, and outputs symbolic compact form for minimal packet size distribution.

She doesn't reason about *what* to build (that's the Oracle). She builds *what she's told*, validates it, and ships it in the smallest possible form.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role architect` |
| Color | Teal (`#38ada9`) |
| Executes programs | Dry-run validation only |
| Signs programs | No |
| Votes on data | No |
| Embeds Nebula | No |
| LLM | **Required** (`--llm` must point to NML-trained model) |

## Responsibilities

### 1. Program Generation

The Architect takes a spec and generates valid NML:

```bash
curl -X POST http://architect:9005/build \
  -H "Content-Type: application/json" \
  -d '{
    "intent": "Detect fraudulent credit card transactions",
    "features": 6,
    "architecture": "6→8→1",
    "data_bindings": {
      "@training_data": "Transaction feature matrix",
      "@training_labels": "Fraud labels (0/1)"
    },
    "training": {"epochs": 1000, "learning_rate": 0.01},
    "threshold": 0.5,
    "output_syntax": "symbolic"
  }'
```

The LLM is specifically the **NML-trained model** (7B, 440K training pairs, 91% grammar accuracy). It generates symbolic syntax directly — no conversion needed.

### 2. Symbolic Output

The Architect requests symbolic syntax from the LLM for minimal packet size:

| Syntax | Example | Size |
|--------|---------|------|
| Classic | `LD R1 @w1` | 1,985 B |
| Symbolic | `↓ κ @w1` | 340 B |
| Compact | `↓ κ @w1¶↓ λ @b1¶...` | 340 B (single line) |

The NML LLM is trained on all three syntaxes and can generate symbolic directly. The Architect ships **symbolic compact** (pilcrow-delimited) for UDP distribution — an entire fraud detection program fits in one packet.

### 3. Validation

Before shipping, the Architect validates by assembling with `nml-crypto`:

```
nml-crypto program.nml --max-cycles 0   (dry-run: assemble only, don't execute)
```

If symbolic validation fails, the Architect retries in classic syntax and converts to symbolic using the opcode/register maps from `nml_builder.py`:

```
OPCODE_MAP:   MMUL→×  MADD→⊕  LD→↓  ST→↑  SIGM→σ  HALT→◼  ...
REGISTER_MAP: R0→ι  R1→κ  R2→λ  RA→α  RB→β  RE→φ  ...
```

### 4. Program Catalog

The Architect maintains a catalog of all programs she's built:

```bash
curl http://architect:9005/catalog
```

Each entry includes: hash, intent, status (valid/invalid), byte size, UDP fitness, timestamp.

## The Pipeline

```
Oracle                     Architect                Sentient              Workers
  │                           │                        │                    │
  │  POST /spec               │                        │                    │
  │  "EU fraud detection"     │                        │                    │
  │  ──────────────────────►  │                        │                    │
  │                           │  NML LLM → symbolic    │                    │
  │                           │  validate (assemble)   │                    │
  │                           │  compact (340 bytes)   │                    │
  │                           │  ─── POST /submit ───► │                    │
  │                           │                        │  sign (Ed25519)    │
  │                           │                        │  ── broadcast ──►  │
  │                           │                        │                    │  execute
  │                           │                        │                    │  VOTE
```

## Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/build` | POST | Build NML from spec (generates + validates) |
| `/validate` | POST | Validate an existing NML program |
| `/catalog` | GET | List all built programs |

## Dashboard View

The Architect dashboard shows:

- **Architect section** — programs built, catalog size, LLM status
- **Catalog** — list of built programs with hash, validity, byte size, intent
- **Oracle section** — Ask panel (if oracle available)
- **Canvas** — teal glow, "building" label
- **Bottom bar** — hidden (architect doesn't execute or vote)

## Starting an Architect

```bash
# LLM is required — must point to NML-trained model
python3 serve/nml_collective.py --name daedalus --port 9005 \
    --seeds http://localhost:9001 --role architect --llm http://localhost:8082
```

## Design Principle

The Architect is a translator, not a thinker. She converts structured intent into valid machine code in the smallest possible form. The Oracle decides *what* to build. The Sentient decides *whether to trust it*. The Architect just builds — precisely, compactly, and correctly.
