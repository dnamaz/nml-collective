#!/bin/bash
# ═══════════════════════════════════════════════════════════
# NML Architect Demo — Full Pipeline
# ═══════════════════════════════════════════════════════════
# Demonstrates the complete program lifecycle:
#   Oracle → Architect → Sentient → Workers
#
# 1. Oracle reasons about what program is needed (spec)
# 2. Architect generates symbolic NML via the NML LLM
# 3. Sentient signs and broadcasts
# 4. Workers execute and VOTE
#
# Requires: NML LLM server running (--llm)
#
# Usage:
#   bash demos/architect_demo.sh --llm=http://localhost:8082
# ═══════════════════════════════════════════════════════════

set -e
cd "$(dirname "$0")/.."

LLM_URL=""
PIDS=()

for arg in "$@"; do
    case $arg in
        --llm=*) LLM_URL="${arg#*=}" ;;
    esac
done

if [ -z "$LLM_URL" ]; then
    echo "Usage: bash demos/architect_demo.sh --llm=http://localhost:8082"
    echo ""
    echo "The Architect needs an NML LLM server to generate programs."
    echo "Start one with: python3 serve/nml_server.py --http --port 8082 --model <model_path>"
    exit 1
fi

cleanup() {
    echo ""
    echo "Stopping collective..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null
    done
    wait 2>/dev/null
    echo "Done."
}
trap cleanup EXIT INT TERM

echo "═══════════════════════════════════════════════════════"
echo "  NML Architect Demo — Oracle → Architect → Sentient → Workers"
echo "═══════════════════════════════════════════════════════"
echo "  LLM: $LLM_URL"

# Start Sentient
echo ""
echo "[SENTIENT] Starting prime on :9001"
python3 serve/nml_collective.py \
    --name prime --port 9001 \
    --role sentient \
    --data demos/agent1.nml.data \
    --no-mdns &
PIDS+=($!)
sleep 2

# Start Workers
echo "[WORKER] Starting worker_us on :9002"
python3 serve/nml_collective.py \
    --name worker_us --port 9002 \
    --seeds http://localhost:9001 \
    --role worker \
    --data demos/agent2.nml.data \
    --no-mdns &
PIDS+=($!)

echo "[WORKER] Starting worker_eu on :9003"
python3 serve/nml_collective.py \
    --name worker_eu --port 9003 \
    --seeds http://localhost:9001 \
    --role worker \
    --data demos/agent3.nml.data \
    --no-mdns &
PIDS+=($!)
sleep 2

# Start Oracle
echo "[ORACLE] Starting sibyl on :9004"
python3 serve/nml_collective.py \
    --name sibyl --port 9004 \
    --seeds http://localhost:9001,http://localhost:9002,http://localhost:9003 \
    --role oracle \
    --llm "$LLM_URL" \
    --no-mdns &
PIDS+=($!)
sleep 2

# Start Architect
echo "[ARCHITECT] Starting daedalus on :9005"
python3 serve/nml_collective.py \
    --name daedalus --port 9005 \
    --seeds http://localhost:9001,http://localhost:9004 \
    --role architect \
    --llm "$LLM_URL" \
    --no-mdns &
PIDS+=($!)
sleep 3

# Verify mesh
echo ""
echo "━━━ Mesh Status ━━━"
for port in 9001 9002 9003 9004 9005; do
    echo -n "  :$port  "
    curl -s http://localhost:$port/health 2>/dev/null || echo "unreachable"
    echo
done

# Step 1: Oracle generates a program spec
echo ""
echo "━━━ Step 1: Oracle generates program spec ━━━"
SPEC=$(curl -s -X POST http://localhost:9004/spec \
    -H "Content-Type: application/json" \
    -d '{"intent": "Detect fraudulent credit card transactions using 6 features"}' 2>/dev/null)
echo "$SPEC" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f\"  Intent: {d.get('intent','?')}\")
print(f\"  Architecture: {d.get('architecture','?')}\")
print(f\"  Features: {d.get('features','?')}\")
print(f\"  Output syntax: {d.get('output_syntax','?')}\")
" 2>/dev/null

# Step 2: Architect builds the program
echo ""
echo "━━━ Step 2: Architect builds symbolic NML ━━━"
BUILD=$(curl -s -X POST http://localhost:9005/build \
    -H "Content-Type: application/json" \
    -d "$SPEC" 2>/dev/null)
echo "$BUILD" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f\"  Status: {d.get('status','?')}\")
print(f\"  Hash: {d.get('hash','?')}\")
print(f\"  Size: {d.get('byte_size','?')} bytes\")
print(f\"  Fits UDP: {d.get('fits_udp','?')}\")
if d.get('validation'):
    v = d['validation']
    print(f\"  Valid: {v.get('valid','?')}\")
    if v.get('errors'):
        for e in v['errors']:
            print(f\"    Error: {e}\")
if d.get('symbolic_compact'):
    sc = d['symbolic_compact']
    print(f\"  Symbolic compact ({len(sc)} chars): {sc[:120]}...\")
" 2>/dev/null

# Step 3: Submit to sentient for signing and broadcast
echo ""
echo "━━━ Step 3: Submit to collective ━━━"
PROGRAM=$(echo "$BUILD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('program',''))" 2>/dev/null)
if [ -n "$PROGRAM" ]; then
    HASH=$(curl -s -X POST http://localhost:9001/submit \
        -H "Content-Type: application/json" \
        -d "{\"program\": $(echo "$PROGRAM" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))')}" \
        | python3 -c "import sys,json; print(json.load(sys.stdin).get('hash','?'))" 2>/dev/null)
    echo "  Submitted, hash: $HASH"
    echo "  Waiting for execution..."
    sleep 6
fi

# Step 4: Consensus
echo ""
echo "━━━ Step 4: VOTE Consensus ━━━"
curl -s -X POST http://localhost:9004/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy":"median"}' 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
if d.get('consensus') is not None:
    print(f\"  Strategy: {d['strategy']}\")
    print(f\"  Consensus: {d['consensus']:.4f}\")
    print(f\"  Agents: {d['count']}\")
    if d.get('assessment'):
        a = d['assessment']
        print(f\"  Confidence: {a['confidence']} — {a['confidence_reason']}\")
else:
    print(f\"  {d.get('error', 'No results')}\")
" 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Pipeline complete: Oracle → Architect → Sentient → Workers → VOTE"
echo ""
echo "  Dashboards:"
echo "    Sentient:  http://localhost:9001/dashboard"
echo "    Worker:    http://localhost:9002/dashboard"
echo "    Oracle:    http://localhost:9004/dashboard"
echo "    Architect: http://localhost:9005/dashboard"
echo ""
echo "  Build another program:"
echo "    curl -X POST http://localhost:9005/build \\"
echo "      -H 'Content-Type: application/json' \\"
echo "      -d '{\"intent\": \"anomaly detection on sensor data\", \"features\": 4}'"
echo ""
echo "  Press Ctrl+C to stop all agents."
echo "═══════════════════════════════════════════════════════"

wait
