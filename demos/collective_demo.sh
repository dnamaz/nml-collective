#!/bin/bash
# ═══════════════════════════════════════════════════════════
# NML Collective Demo — Autonomous Agent Mesh
# ═══════════════════════════════════════════════════════════
# Starts a collective of autonomous NML agents that:
#   1. Discover each other via gossip (seed list)
#   2. Broadcast signed programs peer-to-peer
#   3. Train locally with TNET on their own data
#   4. Reach consensus via VOTE
#   5. Accept new agents dynamically
#
# Usage:
#   bash demos/collective_demo.sh                # 3 agents + dashboard
#   bash demos/collective_demo.sh --with-llm     # includes central LLM server
# ═══════════════════════════════════════════════════════════

set -e
cd "$(dirname "$0")/.."

WITH_LLM=false
MODEL_PATH="domain/output/model/nml-next-merged"
DASHBOARD_PORT=3000
PIDS=()

for arg in "$@"; do
    case $arg in
        --with-llm) WITH_LLM=true ;;
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
echo "  NML Collective — Autonomous Agent Mesh"
echo "═══════════════════════════════════════════════════════"

# Build if needed
if [ ! -f "./nml-crypto" ]; then
    echo ""
    echo "[BUILD] Building nml-crypto..."
    make nml-crypto 2>&1 | tail -1
fi

# Optional: Start central LLM server
if [ "$WITH_LLM" = true ]; then
    echo ""
    echo "[LLM] Starting NML server with model on :8082..."
    python3 serve/nml_server.py --http --port 8082 --model "$MODEL_PATH" &
    PIDS+=($!)
    sleep 5
    LLM_FLAG="--llm http://localhost:8082"
else
    LLM_FLAG=""
fi

# Start Agent 1 (first node — no seeds)
echo ""
echo "[AGENT] Starting agent_1 on :9001 (seed node)"
python3 serve/nml_collective.py \
    --name agent_1 --port 9001 \
    --data demos/agent1.nml.data \
    --key deadbeef01020304 \
    $LLM_FLAG &
PIDS+=($!)
sleep 2

# Start Agent 2 (seeds from agent_1)
echo "[AGENT] Starting agent_2 on :9002 (seeds: 9001)"
python3 serve/nml_collective.py \
    --name agent_2 --port 9002 \
    --seeds http://localhost:9001 \
    --data demos/agent2.nml.data \
    --key deadbeef05060708 \
    $LLM_FLAG &
PIDS+=($!)
sleep 2

# Start Agent 3 (seeds from both)
echo "[AGENT] Starting agent_3 on :9003 (seeds: 9001, 9002)"
python3 serve/nml_collective.py \
    --name agent_3 --port 9003 \
    --seeds http://localhost:9001,http://localhost:9002 \
    --data demos/agent3.nml.data \
    --key deadbeef09101112 \
    $LLM_FLAG &
PIDS+=($!)
sleep 3

# Verify gossip mesh
echo ""
echo "━━━ Mesh Status ━━━"
for port in 9001 9002 9003; do
    PEERS=$(curl -s http://localhost:$port/state 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['peer_count'])" 2>/dev/null || echo "?")
    echo "  Agent on :$port — $PEERS peers"
done

# Serve dashboard
echo ""
echo "━━━ Dashboard ━━━"
echo "  Starting dashboard on :$DASHBOARD_PORT"
python3 -m http.server $DASHBOARD_PORT --directory terminal --bind 127.0.0.1 &>/dev/null &
PIDS+=($!)
sleep 1

# Submit a program to the collective
echo ""
echo "━━━ Broadcasting fraud detection program ━━━"
PROGRAM=$(cat programs/fraud_detection.nml)
HASH=$(curl -s -X POST http://localhost:9001/submit \
    -H "Content-Type: application/json" \
    -d "{\"program\": $(echo "$PROGRAM" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))')}" \
    | python3 -c "import sys,json; print(json.load(sys.stdin).get('hash','?'))" 2>/dev/null)
echo "  Submitted to agent_1, hash: $HASH"
echo "  Broadcasting to collective..."
sleep 4

# Check execution results
echo ""
echo "━━━ Execution Results ━━━"
for port in 9001 9002 9003; do
    SCORE=$(curl -s http://localhost:$port/results 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
for r in d.get('results',[]):
    if 'score' in r:
        print(f\"  Agent on :{$port} — score: {r['score']:.4f}\")
        break
else:
    print(f'  Agent on :$port — no score')
" 2>/dev/null || echo "  Agent on :$port — unreachable")
    echo "$SCORE"
done

# Request consensus
echo ""
echo "━━━ VOTE Consensus ━━━"
CONSENSUS=$(curl -s -X POST http://localhost:9001/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy": "median"}' 2>/dev/null)
echo "$CONSENSUS" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f\"  Strategy: {d.get('strategy','?')}\")
print(f\"  Agents: {d.get('count','?')}\")
print(f\"  Consensus: {d.get('consensus','?')}\")
for a in d.get('agents',[]):
    print(f\"    {a['agent']}: {a['score']:.4f}\")
" 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Collective is running."
echo ""
echo "  Dashboard: http://localhost:$DASHBOARD_PORT/nml_collective_dashboard.html"
echo "    → Enter http://localhost:9001 and click Connect"
echo ""
echo "  Add a 4th agent:"
echo "    python3 serve/nml_collective.py --name agent_4 --port 9004 \\"
echo "        --seeds http://localhost:9001 --data demos/agent1.nml.data"
echo ""
echo "  Submit a program:"
echo "    curl -X POST http://localhost:9001/submit \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"program\": \"LEAF R0 #42.0\\nST R0 @result\\nHALT\"}'"
echo ""
echo "  Press Ctrl+C to stop all agents."
echo "═══════════════════════════════════════════════════════"

# Keep running until Ctrl+C
wait
