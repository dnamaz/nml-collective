#!/bin/bash
# NML Collective — Full Stack Demo
#
# Brings up the complete 8-role collective, runs the full pipeline,
# and shows live output from each layer.
#
# Roles started:
#   Herald    — MQTT broker (Mosquitto supervisor)
#   Sentient  — program authority, data signing, HTTP data server
#   Custodian — data ingestion, quarantine, NebulaDisk storage
#   Worker×3  — NML execution, result reporting via MQTT
#   Enforcer  — identity checks, rate limits, quarantine gossip
#   Oracle    — z-score consensus, per-agent confidence weights
#   Architect — template-based program generation
#   Emissary  — external REST API, webhook delivery
#
# Usage:
#   bash demos/demo.sh
#   bash demos/demo.sh --llm-host=http://localhost:8082
#   BROKER_HOST=192.168.1.10 bash demos/demo.sh   (remote broker)

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

LLM_HOST=""
for arg in "$@"; do
    case $arg in
        --llm-host=*) LLM_HOST="${arg#*=}" ;;
    esac
done

echo "═══════════════════════════════════════════════════════"
echo "  NML Collective — Full Stack Demo"
echo "═══════════════════════════════════════════════════════"
[[ -n "$LLM_HOST" ]] && echo "  LLM: $LLM_HOST"
echo ""

nml_check_build

# ── Phase 1: Infrastructure ───────────────────────────────
echo "━━━ Phase 1: Starting infrastructure ━━━"
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
nml_start oracle    sibyl    9020
if [[ -n "$LLM_HOST" ]]; then
    nml_start architect daedalus 9005 --llm-host "$LLM_HOST"
else
    nml_start architect daedalus 9005
fi
nml_start emissary  gateway  8080 --api-key "demo-key"
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
echo ""
echo "━━━ Phase 5: Architect generates program ━━━"
BUILD=$(curl -s -X POST http://localhost:9005/generate \
    -H "Content-Type: application/json" \
    -d '{"intent": "fraud detection on credit card transactions", "features": 6}' 2>/dev/null)
echo "$BUILD" | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(f'  Template:  {d.get(\"template_id\", d.get(\"provenance\", \"?\"))}')
print(f'  Hash:      {d.get(\"hash\",\"?\")}')
print(f'  Validated: {d.get(\"validated\",\"?\")}')
" 2>/dev/null || echo "  (using pre-built program)"

# ── Phase 6: Submit program to collective ─────────────────
echo ""
echo "━━━ Phase 6: Submit program — Sentient signs and broadcasts ━━━"
PROGRAM=$(echo "$BUILD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('program',''))" 2>/dev/null)
if [[ -n "$PROGRAM" ]]; then
    RESP=$(echo "$PROGRAM" | curl -s -X POST http://localhost:9001/program \
        -H "Content-Type: text/plain" --data-binary @- 2>/dev/null)
    echo "$RESP" | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(f'  hash={d.get(\"hash\",\"?\")}  status={d.get(\"status\",\"?\")}')
" 2>/dev/null || echo "  Submitted"
else
    echo "  Falling back to demo program..."
    nml_submit_program 9001 demos/fraud_detection.nml
fi

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
