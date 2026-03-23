#!/bin/bash
# NML Oracle Demo — Knowledge Layer for the Collective
#
# Starts: sentient + 2 workers + oracle
# Oracle observes everything and answers questions via /ask.
#
# Usage:
#   bash demos/oracle_demo.sh
#   bash demos/oracle_demo.sh --llm=http://localhost:8082

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

LLM_URL=""
for arg in "$@"; do
    case $arg in
        --with-llm)  LLM_URL="http://localhost:8082" ;;
        --llm=*)     LLM_URL="${arg#*=}" ;;
    esac
done

echo "═══════════════════════════════════════════════════════"
echo "  NML Oracle Demo — The Collective's Knowledge Layer"
echo "═══════════════════════════════════════════════════════"
[[ -n "$LLM_URL" ]] && echo "  LLM: $LLM_URL"

nml_start sentient oracle_prime 9001 --data demos/agent1.nml.data --llm "$LLM_URL"; sleep 2
nml_start worker   worker_us   9002 --seeds 9001 --data demos/agent2.nml.data; sleep 2
nml_start worker   worker_eu   9003 --seeds 9001 --data demos/agent3.nml.data; sleep 2
nml_start oracle   sibyl       9004 --seeds 9001,9002,9003 --llm "$LLM_URL"; sleep 3

nml_check_mesh 9001 9002 9003 9004

echo ""
echo "━━━ Broadcasting fraud detection program ━━━"
nml_submit_program 9001 demos/fraud_detection.nml
sleep 5

echo ""
echo "━━━ Asking the Oracle ━━━"
echo ""
echo "  Q: How many agents are in the collective?"
curl -s -X POST http://localhost:9004/ask \
    -H "Content-Type: application/json" \
    -d '{"question": "How many agents are in the collective and what are their roles?"}' 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'  A: {d.get(\"answer\",\"no answer\")}')" 2>/dev/null

echo ""
echo "  Q: What is the status of each agent?"
curl -s -X POST http://localhost:9004/ask \
    -H "Content-Type: application/json" \
    -d '{"question": "What is the status of each agent?"}' 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'  A: {d.get(\"answer\",\"no answer\")}')" 2>/dev/null

echo ""
echo "━━━ Oracle Context ━━━"
curl -s http://localhost:9004/context 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f'  Collective size: {d.get(\"collective_size\", 0)}')
print(f'  Events tracked:  {d.get(\"events_tracked\", 0)}')
print(f'  Programs known:  {d.get(\"programs_known\", 0)}')
print(f'  LLM connected:   {d.get(\"llm_connected\", False)}')
for a in d.get('agents', []):
    score = f', score={a[\"last_score\"]:.4f}' if a.get('last_score') is not None else ''
    print(f'    {a[\"name\"]:15s} role={a[\"role\"]:10s} peers={a[\"peer_count\"]}{score}')" 2>/dev/null

echo ""
echo "━━━ Oracle Recommendations ━━━"
curl -s http://localhost:9004/recommend 2>/dev/null | python3 -c "
import sys,json
for r in json.load(sys.stdin).get('recommendations', []):
    print(f'  [{r[\"severity\"]:7s}] {r[\"category\"]:10s} — {r[\"message\"]}')" 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Collective is running with Oracle."
echo ""
echo "  Dashboard:  http://localhost:9001/dashboard"
echo "  Oracle API: http://localhost:9004"
echo ""
echo "  Ask the Oracle:"
echo "    curl -X POST http://localhost:9004/ask \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"question\": \"Which agents have executed programs?\"}'"
echo ""
echo "  Add more agents:"
echo "    bash demos/agent.sh worker --name w_asia --port 9005 --seeds 9001"
echo ""
echo "  Press Ctrl+C to stop all agents."
echo "═══════════════════════════════════════════════════════"

wait
