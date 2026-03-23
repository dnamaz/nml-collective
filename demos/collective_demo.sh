#!/bin/bash
# NML Collective Demo — Autonomous Agent Mesh
#
# Starts 3 worker agents that discover each other, broadcast programs, and VOTE.
#
# Usage:
#   bash demos/collective_demo.sh
#   bash demos/collective_demo.sh --with-llm

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

WITH_LLM=false
LLM_URL=""
MODEL_PATH="domain/output/model/nml-next-merged"
for arg in "$@"; do [[ $arg == --with-llm ]] && WITH_LLM=true; done

echo "═══════════════════════════════════════════════════════"
echo "  NML Collective — Autonomous Agent Mesh"
echo "═══════════════════════════════════════════════════════"

nml_check_build

if [ "$WITH_LLM" = true ]; then
    echo "[LLM] Starting NML server on :8082..."
    python3 serve/nml_server.py --http --port 8082 --model "$MODEL_PATH" &
    PIDS+=($!)
    sleep 5
    LLM_URL="http://localhost:8082"
fi

nml_start worker agent_1 9001 --data demos/agent1.nml.data --key deadbeef01020304 --llm "$LLM_URL"; sleep 2
nml_start worker agent_2 9002 --seeds 9001 --data demos/agent2.nml.data --key deadbeef05060708 --llm "$LLM_URL"; sleep 2
nml_start worker agent_3 9003 --seeds 9001,9002 --data demos/agent3.nml.data --key deadbeef09101112 --llm "$LLM_URL"; sleep 3

nml_check_mesh 9001 9002 9003

echo ""
echo "━━━ Broadcasting fraud detection program ━━━"
nml_submit_program 9001 demos/fraud_detection.nml
sleep 4

echo ""
echo "━━━ Execution Results ━━━"
for port in 9001 9002 9003; do
    curl -s "http://localhost:$port/results" 2>/dev/null | python3 -c "
import sys,json
for r in json.load(sys.stdin).get('results',[]):
    if 'score' in r:
        print(f'  Agent on :$port — score: {r[\"score\"]:.4f}')
        break
else:
    print('  Agent on :$port — no score')" 2>/dev/null || echo "  Agent on :$port — unreachable"
done

echo ""
echo "━━━ VOTE Consensus ━━━"
curl -s -X POST http://localhost:9001/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy": "median"}' 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f'  Strategy:  {d.get(\"strategy\",\"?\")}')
print(f'  Agents:    {d.get(\"count\",\"?\")}')
print(f'  Consensus: {d.get(\"consensus\",\"?\")}')
for a in d.get('agents',[]): print(f'    {a[\"agent\"]}: {a[\"score\"]:.4f}')" 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Collective is running."
echo ""
echo "  Dashboard: http://localhost:9001/dashboard"
echo ""
echo "  Add more agents:"
echo "    bash demos/agent.sh worker --name my_node --port 9004 --seeds 9001"
echo ""
echo "  Submit a program:"
echo "    curl -X POST http://localhost:9001/submit \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"program\": \"LEAF R0 #42.0\nST R0 @result\nHALT\"}'"
echo ""
echo "  Press Ctrl+C to stop all agents."
echo "═══════════════════════════════════════════════════════"

wait
