# Role: Architect

The builder of the collective. The Architect receives structured program specifications (from the Oracle), selects or generates valid NML programs, validates them by dry-run assembly, and outputs symbolic compact form for minimal packet size distribution.

She doesn't reason about *what* to build (that's the Oracle). She builds *what she's told*, validates it, and ships it in the smallest possible form. For the vast majority of requests she never touches the LLM — she selects from a template library and fills in the parameters.

## Identity

| Property | Value |
|----------|-------|
| Flag | `--role architect` |
| Color | Teal (`#38ada9`) |
| Executes programs | Dry-run validation only |
| Signs programs | No |
| Votes | Yes — did the program behave as designed? Score in expected range? |
| Embeds Nebula | No |
| LLM | Optional — fallback for novel architectures only |

## Template Library

Most prediction tasks fit one of five standard patterns. The Architect maintains a **template library** — parameterized NML programs that are instantiated by filling in dimensions, not by calling the LLM.

### Standard Templates

| Template | Intent Pattern | Output | Shape |
|----------|---------------|--------|-------|
| `binary_classifier` | yes/no, true/false, above/below, win/lose | sigmoid → threshold | N→H→1 |
| `multiclass_classifier` | which one, categorize, select from K options | softmax → argmax | N→H→K |
| `regression` | how much, what value, predict a number | linear | N→H→1 |
| `ranking` | order these, top N, rank items | score per item → sort | N→H→1 per item |
| `anomaly_detector` | is this normal, detect outliers | reconstruction error | N→H→N |

### Template Parameters

Each template accepts:

```
input_dim       — number of features (from Oracle data spec)
hidden_dims[]   — hidden layer sizes (from Oracle or Architect default)
threshold       — decision boundary (binary_classifier, anomaly_detector)
output_key      — named memory key for result extraction
learning_rate   — TNET training rate
epochs          — TNET training cycles
```

### NML Instruction Sequence

Every template produces the same fundamental instruction sequence — only the register sizes and tensor dimensions change:

```
; binary_classifier(input_dim=12, hidden=[16,8], threshold=0.5)

↓ κ @w1        ; load weights [12×16]
↓ λ @b1        ; load biases  [16]
↓ μ @w2        ; load weights [16×8]
↓ ν @b2        ; load biases  [8]
↓ ξ @w3        ; load weights [8×1]
↓ ο @b3        ; load biases  [1]
↓ α @data      ; load input data

◼ TNET α κ λ   ; train layer 1
◼ TNET α μ ν   ; train layer 2
◼ TNET α ξ ο   ; train layer 3

× κ α → β      ; MMUL forward layer 1
⊕ β λ → β      ; MADD + bias
ρ β → β         ; RELU activation

× μ β → γ      ; MMUL forward layer 2
⊕ γ ν → γ      ; MADD + bias
ρ γ → γ         ; RELU activation

× ξ γ → δ      ; MMUL forward layer 3
⊕ δ ο → δ      ; MADD + bias
σ δ → δ         ; SIGM output

⊕ δ "win_probability" → φ   ; store in named memory
◼                             ; HALT
```

The same sequence with `input_dim=6, hidden=[8]` is the fraud detection program. With `input_dim=68, hidden=[32,16]` it's the March Madness program. The template is identical — the Architect just fills in the numbers.

### Template Selection Flow

```
Oracle spec: "binary classification, 12 features, threshold 0.5"
                              │
                    ┌─────────▼──────────┐
                    │ Template exists?    │
                    └─────────┬──────────┘
                         ╱          ╲
                       yes           no
                        │             │
              Fill template      Call LLM
              (microseconds)     (seconds)
                        │             │
                        ▼             ▼
                   NML program    NML program
                        │             │
                        └──────┬──────┘
                               │
                         Validate (dry-run)
                               │
                         Ship to Sentient
```

### When the LLM is needed

The LLM is a fallback, not the primary path:

| Situation | Template? | LLM? |
|-----------|-----------|------|
| Predict win/lose | binary_classifier | No |
| Predict a price | regression | No |
| Rank 68 teams | ranking | No |
| Detect fraud | binary_classifier | No |
| Find anomalous sensor readings | anomaly_detector | No |
| Novel multi-task architecture | — | Yes |
| Custom loss function | — | Yes |
| Unusual data shape (graph, sequence) | — | Yes |
| Template failed validation | Retry | Yes, generate from scratch |

90% of requests are template instantiation. The Architect is fast by default.

## Responsibilities

### 1. Template Instantiation (primary path)

The Oracle sends a spec with enough information to select and fill a template:

```json
{
  "template": "binary_classifier",
  "input_dim": 12,
  "hidden_dims": [16, 8],
  "threshold": 0.5,
  "output_key": "win_probability",
  "learning_rate": 0.01,
  "epochs": 1000
}
```

The Architect selects the template, substitutes parameters, and produces a valid NML program in microseconds.

### 2. LLM Generation (fallback path)

When no template fits, the Architect calls the NML-trained LLM:

```json
{
  "intent": "Multi-task: classify fraud AND predict transaction amount",
  "features": 10,
  "outputs": ["fraud_score", "predicted_amount"],
  "note": "shared hidden layers, two output heads"
}
```

The LLM is specifically the **NML-trained model** (7B, 440K training pairs, 91% grammar accuracy). It generates symbolic syntax directly.

### 3. Symbolic Output

The Architect ships **symbolic compact** (pilcrow-delimited) for minimal packet size:

| Syntax | Example | Size |
|--------|---------|------|
| Classic | `LD R1 @w1` | 1,985 B |
| Symbolic | `↓ κ @w1` | 340 B |
| Compact | `↓ κ @w1¶↓ λ @b1¶...` | 340 B (single line) |

An entire fraud detection program fits in one MQTT message.

### 4. Validation

Before shipping, the Architect validates by dry-run assembly:

```
nml-crypto program.nml --max-cycles 0   (assemble only, don't execute)
```

If symbolic validation fails, the Architect retries in classic syntax and converts to symbolic using the opcode/register maps:

```
OPCODE_MAP:   MMUL→×  MADD→⊕  LD→↓  ST→↑  SIGM→σ  HALT→◼  ...
REGISTER_MAP: R0→ι  R1→κ  R2→λ  RA→α  RB→β  RE→φ  ...
```

### 5. Program Catalog

The Architect maintains a catalog of all programs — both templated and LLM-generated:

Each entry includes: hash, template (or "llm"), intent, parameters, status (valid/invalid), byte size, timestamp.

### 6. Vote

The Architect votes on consensus with its own criteria:
- Did the program produce a score in the expected range for this architecture?
- Did execution complete cleanly (no TRAP, no timeout)?
- Is the output key present in the result?

A score outside the expected range (e.g. sigmoid output > 1.0) suggests corruption, not just a different perspective.

## The Pipeline

```
Oracle                     Architect                Sentient              Workers
  │                           │                        │                    │
  │  nml/spec                 │                        │                    │
  │  "binary, 12 features"   │                        │                    │
  │  ──────────────────────►  │                        │                    │
  │                           │  template? yes         │                    │
  │                           │  fill binary_classifier│                    │
  │                           │  validate (dry-run)    │                    │
  │                           │  compact (340 bytes)   │                    │
  │                           │  ─── nml/submit ─────► │                    │
  │                           │                        │  sign (Ed25519)    │
  │                           │                        │  ── nml/program ─► │
  │                           │                        │                    │  execute
  │                           │                        │                    │  vote
```

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `nml/spec` | Subscribe | Receive program specs from Oracle |
| `nml/submit` | Publish | Submit validated program to Sentient |
| `nml/vote/<phash>` | Publish | Vote on consensus (structural assessment) |

## Design Principle

The Architect is a template engine with an LLM escape hatch. For known patterns she is instant — select template, fill parameters, validate, ship. For novel architectures she falls back to the LLM. She converts structured intent into valid machine code in the smallest possible form. The Oracle decides *what* to build. The Sentient decides *whether to trust it*. The Architect just builds — precisely, compactly, and correctly.
