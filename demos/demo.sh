#!/bin/bash
# NML Collective — Full Stack Demo
#
# Brings up the complete 8-role collective, runs the full pipeline,
# and shows live output from each layer.
#
# Three-tier LLM pipeline (oracle + architect):
#   Stage 1 — External cloud model (OpenRouter, e.g. Haiku) provides deep context
#   Stage 2 — Local NML think model (nml-think-v2-4bit) for NML-specific reasoning
#   Stage 3 — Local NML code model (nml-v09-merged-6bit) for exact code generation
#
# Before running with local models, start the mlx_lm servers:
#   cd nml-model-training
#   python -m mlx_lm.server --model models/nml-think-v2-4bit \
#       --adapter mlx_training_think/adapters --port 8084 &
#   python -m mlx_lm.server --model models/nml-v09-merged-6bit --port 8085 &
#
# Usage:
#   bash demos/demo.sh                          (templates only, no LLM)
#   bash demos/demo.sh --llm-api-key=sk-or-v1-...
#       (OpenRouter Haiku as external stage, no local models)
#   bash demos/demo.sh --llm-api-key=sk-or-v1-... \
#                      --think-host=localhost --think-port=8084 \
#                      --think-model=nml-think-v2-4bit \
#                      --code-host=localhost  --code-port=8085 \
#                      --code-model=nml-v09-merged-6bit
#       (full three-tier: OpenRouter + local think + local code)
#   bash demos/demo.sh --llm-api-key=sk-or-v1-... --llm-model=openai/gpt-4o
#       (override OpenRouter model)
#   bash demos/demo.sh --llm-host=127.0.0.1 --llm-port=8080 \
#                      --llm-path=/v1/chat/completions   (fully local LLM, no key)
#   BROKER_HOST=192.168.1.10 bash demos/demo.sh   (remote broker)

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

LLM_HOST="openrouter.ai"
LLM_PORT="443"
LLM_PATH="/api/v1/chat/completions"
LLM_API_KEY=""
LLM_MODEL="anthropic/claude-haiku-4-5-20251001"  # external stage (OpenRouter)
THINK_HOST=""                                      # local nml-think-v2-4bit
THINK_PORT="8084"
CODE_HOST=""                                       # local nml-v09-merged-6bit
CODE_PORT="8085"
LOG_FILE=""

# mlx_lm.server exposes models by their full absolute path — compute at runtime
_NML_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
THINK_MODEL="${_NML_ROOT}/nml-model-training/models/nml-think-v2-4bit"
CODE_MODEL="${_NML_ROOT}/nml-model-training/models/nml-v09-merged-6bit"
_args=("$@")
_i=0
while (( _i < ${#_args[@]} )); do
    _arg="${_args[$_i]}"
    case "$_arg" in
        --llm-host=*)      LLM_HOST="${_arg#*=}" ;;
        --llm-host)        _i=$(( _i+1 )); LLM_HOST="${_args[$_i]}" ;;
        --llm-port=*)      LLM_PORT="${_arg#*=}" ;;
        --llm-port)        _i=$(( _i+1 )); LLM_PORT="${_args[$_i]}" ;;
        --llm-path=*)      LLM_PATH="${_arg#*=}" ;;
        --llm-path)        _i=$(( _i+1 )); LLM_PATH="${_args[$_i]}" ;;
        --llm-api-key=*)   LLM_API_KEY="${_arg#*=}" ;;
        --llm-api-key)     _i=$(( _i+1 )); LLM_API_KEY="${_args[$_i]}" ;;
        --llm-model=*)     LLM_MODEL="${_arg#*=}" ;;
        --llm-model)       _i=$(( _i+1 )); LLM_MODEL="${_args[$_i]}" ;;
        --think-host=*)    THINK_HOST="${_arg#*=}" ;;
        --think-host)      _i=$(( _i+1 )); THINK_HOST="${_args[$_i]}" ;;
        --think-port=*)    THINK_PORT="${_arg#*=}" ;;
        --think-port)      _i=$(( _i+1 )); THINK_PORT="${_args[$_i]}" ;;
        --think-model=*)   THINK_MODEL="${_arg#*=}" ;;
        --think-model)     _i=$(( _i+1 )); THINK_MODEL="${_args[$_i]}" ;;
        --code-host=*)     CODE_HOST="${_arg#*=}" ;;
        --code-host)       _i=$(( _i+1 )); CODE_HOST="${_args[$_i]}" ;;
        --code-port=*)     CODE_PORT="${_arg#*=}" ;;
        --code-port)       _i=$(( _i+1 )); CODE_PORT="${_args[$_i]}" ;;
        --code-model=*)    CODE_MODEL="${_arg#*=}" ;;
        --code-model)      _i=$(( _i+1 )); CODE_MODEL="${_args[$_i]}" ;;
        --log-file=*)      LOG_FILE="${_arg#*=}" ;;
        --log-file)        _i=$(( _i+1 )); LOG_FILE="${_args[$_i]}" ;;
    esac
    _i=$(( _i+1 ))
done
unset _args _i _arg

# Build reusable flag arrays for agent start calls
_llm_args=()
[[ -n "$LLM_HOST"    ]] && _llm_args+=(--llm-host "$LLM_HOST")
[[ -n "$LLM_PORT"    ]] && _llm_args+=(--llm-port "$LLM_PORT")
[[ -n "$LLM_PATH"    ]] && _llm_args+=(--llm-path "$LLM_PATH")
[[ -n "$LLM_API_KEY" ]] && _llm_args+=(--llm-api-key "$LLM_API_KEY")
[[ -n "$LLM_MODEL"   ]] && _llm_args+=(--llm-model "$LLM_MODEL")
[[ -n "$THINK_HOST"  ]] && _llm_args+=(--think-host "$THINK_HOST")
[[ -n "$THINK_PORT"  ]] && _llm_args+=(--think-port "$THINK_PORT")
[[ -n "$THINK_MODEL" ]] && _llm_args+=(--think-model "$THINK_MODEL")
[[ -n "$CODE_HOST"   ]] && _llm_args+=(--code-host "$CODE_HOST")
[[ -n "$CODE_PORT"   ]] && _llm_args+=(--code-port "$CODE_PORT")
[[ -n "$CODE_MODEL"  ]] && _llm_args+=(--code-model "$CODE_MODEL")

_infer_args=()
[[ -n "$LLM_API_KEY" ]] && _infer_args+=(--llm-api-key "$LLM_API_KEY")
[[ -n "$LLM_PATH"    ]] && _infer_args+=(--llm-path "$LLM_PATH")
[[ -n "$THINK_HOST"  ]] && _infer_args+=(--think-host "$THINK_HOST")
[[ -n "$THINK_PORT"  ]] && _infer_args+=(--think-port "$THINK_PORT")
[[ -n "$THINK_MODEL" ]] && _infer_args+=(--think-model "$THINK_MODEL")
[[ -n "$CODE_HOST"   ]] && _infer_args+=(--code-host "$CODE_HOST")
[[ -n "$CODE_PORT"   ]] && _infer_args+=(--code-port "$CODE_PORT")
[[ -n "$CODE_MODEL"  ]] && _infer_args+=(--code-model "$CODE_MODEL")
# Emissary /infer uses the same OpenRouter host as the code model when no
# separate code host is set
if [[ -z "$CODE_HOST" && -n "$LLM_HOST" ]]; then
    _infer_args+=(--code-host "$LLM_HOST" --code-port "${LLM_PORT:-443}")
fi

echo "═══════════════════════════════════════════════════════"
echo "  NML Collective — Full Stack Demo"
echo "═══════════════════════════════════════════════════════"
echo "  External: ${LLM_HOST}:${LLM_PORT}  model=${LLM_MODEL}"
[[ -n "$THINK_HOST"  ]] && echo "  Think:    ${THINK_HOST}:${THINK_PORT}  model=${THINK_MODEL}"
[[ -n "$CODE_HOST"   ]] && echo "  Code:     ${CODE_HOST}:${CODE_PORT}   model=${CODE_MODEL}"
[[ -n "$LLM_API_KEY" ]] && echo "  Auth:     OpenRouter key set"
[[ -n "$LOG_FILE"    ]] && echo "  Log:      $LOG_FILE"
echo ""

nml_check_build

# ── Phase 1: Infrastructure ───────────────────────────────
echo "━━━ Phase 1: Starting infrastructure ━━━"
# --no-auth disables Mosquitto username/password authentication.
# This is intentional for local development — all traffic stays on 127.0.0.1
# (loopback) and is unencrypted (plain MQTT on port 1883, no TLS).
# For production: remove --no-auth and issue credentials via
#   POST /credentials/issue  (Herald REST API on port 9000)
# then configure TLS (port 8883) in the Herald mosquitto.conf.
nml_start_herald --no-auth

# Wait for Mosquitto to be fully ready (up to 20 seconds)
echo -n "  Waiting for broker..."
for _i in $(seq 1 40); do
    # mosquitto_sub --help test would work, but simplest: just wait
    # for the port AND give Mosquitto time to finish its init
    if (: < /dev/tcp/"$BROKER_HOST"/"$BROKER_PORT") 2>/dev/null; then
        sleep 3   # Mosquitto opens port before it's fully ready for MQTT
        echo " ready"
        break
    fi
    sleep 0.5
done

nml_start sentient  prime    9001
sleep 1
nml_start custodian vault    9010
sleep 2

# ── Phase 2: Compute layer ────────────────────────────────
echo ""
echo "━━━ Phase 2: Starting workers and enforcer ━━━"
nml_start worker    worker_1 9002 --data demos/agent1.nml.data
nml_start worker    worker_2 9003 --data demos/agent2.nml.data
nml_start worker    worker_3 9004 --data demos/agent3.nml.data
sleep 1

# Enforcer has no HTTP port — start it directly
echo "[guardian] Starting enforcer (MQTT daemon)"
"roles/enforcer/enforcer_agent${_EXT}" \
    --name guardian \
    --broker "$BROKER_HOST" --broker-port "$BROKER_PORT" &
PIDS+=($!)
sleep 2

# ── Phase 3: Intelligence + external boundary ─────────────
echo ""
echo "━━━ Phase 3: Starting oracle, architect, emissary ━━━"
nml_start oracle    sibyl    9020 "${_llm_args[@]}" ${LOG_FILE:+--log-file "$LOG_FILE"}
nml_start architect daedalus 9005 "${_llm_args[@]}"
nml_start emissary  gateway  8080 --api-key "demo-key" "${_infer_args[@]}"
sleep 3

# ── Mesh status ───────────────────────────────────────────
nml_check_mesh 9001 9010 9020 9005 8080

# ── Phase 4: Data ingestion (Custodian) ───────────────────
echo ""
echo "━━━ Phase 4: Ingest transaction data via Custodian ━━━"
REGIONS=("us" "eu" "asia")
declare -a HASHES
for i in 0 1 2; do
    region="${REGIONS[$i]}"
    resp=$(curl -s -X POST http://localhost:9010/ingest \
        -H "Content-Type: application/json" \
        -d "{
              \"name\":   \"transactions_${region}\",
              \"author\": \"worker_$((i+1))\",
              \"data\":   [0.12,0.85,0.03,0.15,0.0,0.10,
                          0.25,0.50,0.08,0.25,0.0,0.15,
                          0.08,0.95,0.01,0.10,0.0,0.05,
                          0.30,0.40,0.12,0.35,0.0,0.20,
                          0.18,0.60,0.05,0.20,1.0,0.15],
              \"shape\":  [5, 6]
            }" 2>/dev/null)
    hash=$(echo "$resp" | grep -o '"hash":"[^"]*"' | cut -d'"' -f4)
    HASHES+=("$hash")
    echo "  transactions_${region}  hash=${hash:-?}"
done

echo ""
echo "  Approving all items..."
for hash in "${HASHES[@]}"; do
    [[ -z "$hash" ]] && continue
    curl -s -X POST http://localhost:9010/approve \
        -H "Content-Type: application/json" \
        -d "{\"hash\":\"$hash\",\"reason\":\"schema valid\"}" > /dev/null
    echo "  Approved: $hash"
done

# ── Phase 5: Architect generates a program ────────────────
# Architect uses template engine (primary) or LLM (fallback).
# On success it publishes MSG_PROGRAM via MQTT → Sentient signs → workers execute.
echo ""
echo "━━━ Phase 5: Architect generates and broadcasts program ━━━"
BUILD=$(curl -s --max-time 60 -X POST http://localhost:9005/generate \
    -H "Content-Type: application/json" \
    -d '{"spec": "fraud detection on credit card transactions"}' 2>/dev/null)
echo "$BUILD" | python3 -c "
import sys, json
d = json.load(sys.stdin)
if not d.get('ok'):
    print(f'  ERROR: {d.get(\"error\", \"unknown\")}')
    sys.exit(1)
prov = d.get('provenance', '?')
tid  = d.get('template_id', -1)
prov_str = f'template #{tid}' if tid >= 0 else 'llm'
print(f'  Provenance: {prov_str}')
print(f'  Hash:       {d.get(\"hash\",\"?\")}')
print(f'  Validated:  {d.get(\"validated\",\"?\")}')
print(f'  Spec:       {d.get(\"spec\",\"?\")[:60]}')
print(f'  Broadcast:  MSG_PROGRAM → MQTT → Sentient → Workers')
" 2>/dev/null || echo "  (generation failed or timed out — check architect log)"

# ── Phase 6: Confirm distribution ────────────────────────────────────────────
# Programs reach workers via MQTT (Architect → Sentient → Workers).
# There is no HTTP /program endpoint on Sentient — the MQTT path is the only path.
echo ""
echo "━━━ Phase 6: Waiting for workers to execute ━━━"
echo "  Program broadcast via MQTT (Architect → Sentient → Workers)"

echo "  Waiting for workers to execute..."
sleep 7

# ── Phase 7: Oracle assessment ────────────────────────────
echo ""
echo "━━━ Phase 7: Oracle consensus assessment ━━━"
curl -s http://localhost:9020/assessments 2>/dev/null \
    | python3 -c "
import sys, json
data = json.load(sys.stdin)
entries = data if isinstance(data, list) else data.get('assessments', [])
if not entries:
    print('  (results still propagating)')
for a in entries:
    print(f'  program={a.get(\"phash\",\"?\")[:16]}')
    print(f'    raw mean    = {a.get(\"raw_mean\", 0):.6f}')
    print(f'    weighted    = {a.get(\"weighted_mean\", 0):.6f}')
    print(f'    confidence  = {a.get(\"confidence\", 0):.4f}')
    print(f'    voters      = {a.get(\"vote_count\", 0)}  outliers={a.get(\"outlier_count\",0)}')
" 2>/dev/null

echo ""
echo "━━━ Per-agent confidence weights ━━━"
curl -s http://localhost:9020/agents 2>/dev/null \
    | python3 -c "
import sys, json
data = json.load(sys.stdin)
for a in data if isinstance(data, list) else data.get('agents', []):
    print(f'  {a.get(\"name\",\"?\"):20s}  conf={a.get(\"confidence\",0):.3f}  '
          f'votes={a.get(\"vote_count\",0):3d}  outliers={a.get(\"outlier_count\",0):2d}')
" 2>/dev/null || echo "  (no agent stats yet)"

# ── Phase 8: Custodian pool (data access counts) ──────────
echo ""
echo "━━━ Phase 8: Data pool (served to workers) ━━━"
curl -s http://localhost:9010/pool 2>/dev/null \
    | python3 -c "
import sys, json
data = json.load(sys.stdin)
for it in (data if isinstance(data,list) else data.get('pool',[])):
    print(f'  {it.get(\"name\",\"?\"):25s}  status={it.get(\"status\",\"?\"):10s}  '
          f'served={it.get(\"served_count\",0)}x')
" 2>/dev/null

# ── Running ───────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Collective is running."
echo ""
echo "  Sentient   http://localhost:9001/health"
echo "  Custodian  http://localhost:9010/pool"
echo "  Oracle     http://localhost:9020/assessments"
echo "             http://localhost:9020/agents"
echo "  Architect  http://localhost:9005/catalog"
echo "  Emissary   http://localhost:8080/status          (Bearer demo-key)"
echo ""
echo "  Submit a program:"
echo "    curl -X POST http://localhost:9001/program \\"
echo "        -H 'Content-Type: text/plain' \\"
echo "        --data-binary @demos/fraud_detection.nml"
echo ""
echo "  Generate via Architect:"
echo "    curl -X POST http://localhost:9005/generate \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"intent\": \"anomaly detection on sensor data\", \"features\": 4}'"
echo ""
echo "  Add a worker:"
echo "    bash demos/agent.sh worker --name w_4 --data demos/agent1.nml.data"
echo ""
echo "  Press Ctrl+C to stop."
echo "═══════════════════════════════════════════════════════"

wait
