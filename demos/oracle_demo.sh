#!/bin/bash
# ═══════════════════════════════════════════════════════════
# NML Oracle Demo — Knowledge Layer for the Collective
# ═══════════════════════════════════════════════════════════
# Starts a collective with:
#   1. A sentient agent (signs programs, approves data)
#   2. Two worker agents (execute programs, submit data)
#   3. An oracle agent (observes everything, answers questions)
#
# The Oracle connects to all agents via gossip and builds
# full collective awareness. You can ask her questions via
# the /ask endpoint or through the dashboard.
#
# Usage:
#   bash demos/oracle_demo.sh
#   bash demos/oracle_demo.sh --with-llm    # connect LLM for deep reasoning
# ═══════════════════════════════════════════════════════════

set -e
cd "$(dirname "$0")/.."

WITH_LLM=false
LLM_URL=""
PIDS=()

for arg in "$@"; do
    case $arg in
        --with-llm) WITH_LLM=true ;;
        --llm=*) LLM_URL="${arg#*=}" ;;
    esac
done

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
echo "  NML Oracle Demo — The Collective's Knowledge Layer"
echo "═══════════════════════════════════════════════════════"

if [ "$WITH_LLM" = true ] && [ -z "$LLM_URL" ]; then
    LLM_URL="http://localhost:8082"
fi
LLM_FLAG=""
if [ -n "$LLM_URL" ]; then
    LLM_FLAG="--llm $LLM_URL"
    echo "  LLM: $LLM_URL"
fi

# Start Sentient (oracle for the nebula, signs programs)
echo ""
echo "[SENTIENT] Starting oracle_prime on :9001 (sentient — signs + approves)"
python3 serve/nml_collective.py \
    --name oracle_prime --port 9001 \
    --role sentient \
    --data demos/agent1.nml.data \
    $LLM_FLAG &
PIDS+=($!)
sleep 2

# Start Worker 1
echo "[WORKER] Starting worker_us on :9002 (US data)"
python3 serve/nml_collective.py \
    --name worker_us --port 9002 \
    --seeds http://localhost:9001 \
    --role worker \
    --data demos/agent2.nml.data &
PIDS+=($!)
sleep 2

# Start Worker 2
echo "[WORKER] Starting worker_eu on :9003 (EU data)"
python3 serve/nml_collective.py \
    --name worker_eu --port 9003 \
    --seeds http://localhost:9001 \
    --role worker \
    --data demos/agent3.nml.data &
PIDS+=($!)
sleep 2

# Start the Oracle
echo "[ORACLE] Starting sibyl on :9004 (observes everything, answers questions)"
python3 serve/nml_collective.py \
    --name sibyl --port 9004 \
    --seeds http://localhost:9001,http://localhost:9002,http://localhost:9003 \
    --role oracle \
    $LLM_FLAG &
PIDS+=($!)
sleep 3

# Verify mesh
echo ""
echo "━━━ Mesh Status ━━━"
for port in 9001 9002 9003 9004; do
    STATUS=$(curl -s http://localhost:$port/health 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f\"  :{$port} {d.get('agent','?'):15s} — {d.get('peers',0)} peers, {d.get('programs',0)} programs\")
" 2>/dev/null || echo "  :$port — unreachable")
    echo "$STATUS"
done

# Submit a program to trigger execution across the collective
echo ""
echo "━━━ Broadcasting fraud detection program ━━━"
PROGRAM=$(cat demos/fraud_detection.nml)
HASH=$(curl -s -X POST http://localhost:9001/submit \
    -H "Content-Type: application/json" \
    -d "{\"program\": $(echo "$PROGRAM" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))')}" \
    | python3 -c "import sys,json; print(json.load(sys.stdin).get('hash','?'))" 2>/dev/null)
echo "  Submitted, hash: $HASH"
sleep 5

# Ask the Oracle about the collective
echo ""
echo "━━━ Asking the Oracle ━━━"
echo ""
echo "  Q: How many agents are in the collective?"
ANSWER=$(curl -s -X POST http://localhost:9004/ask \
    -H "Content-Type: application/json" \
    -d '{"question": "How many agents are in the collective and what are their roles?"}' 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(f\"  A: {d.get('answer','no answer')}\")" 2>/dev/null)
echo "$ANSWER"

echo ""
echo "  Q: What is the collective status?"
ANSWER=$(curl -s -X POST http://localhost:9004/ask \
    -H "Content-Type: application/json" \
    -d '{"question": "What is the status of each agent?"}' 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(f\"  A: {d.get('answer','no answer')}\")" 2>/dev/null)
echo "$ANSWER"

# Get Oracle's full context
echo ""
echo "━━━ Oracle Context ━━━"
curl -s http://localhost:9004/context 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f\"  Collective size: {d.get('collective_size', 0)}\")
print(f\"  Events tracked:  {d.get('events_tracked', 0)}\")
print(f\"  Programs known:  {d.get('programs_known', 0)}\")
print(f\"  LLM connected:   {d.get('llm_connected', False)}\")
for a in d.get('agents', []):
    score = f\", score={a['last_score']:.4f}\" if a.get('last_score') is not None else ''
    print(f\"    {a['name']:15s} role={a['role']:10s} peers={a['peer_count']}{score}\")
" 2>/dev/null

# Get recommendations
echo ""
echo "━━━ Oracle Recommendations ━━━"
curl -s http://localhost:9004/recommend 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
for r in d.get('recommendations', []):
    print(f\"  [{r['severity']:7s}] {r['category']:10s} — {r['message']}\")
" 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Collective is running with Oracle."
echo ""
echo "  Dashboard:  http://localhost:9001/dashboard"
echo "  Oracle API: http://localhost:9004"
echo ""
echo "  Ask the Oracle:"
echo "    curl -s -X POST http://localhost:9004/ask \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"question\": \"Which agents have executed programs?\"}'"
echo ""
echo "  Get full context:"
echo "    curl -s http://localhost:9004/context | python3 -m json.tool"
echo ""
echo "  Get recommendations:"
echo "    curl -s http://localhost:9004/recommend | python3 -m json.tool"
echo ""
echo "  Press Ctrl+C to stop all agents."
echo "═══════════════════════════════════════════════════════"

wait
