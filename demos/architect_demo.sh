#!/bin/bash
# NML Architect Demo — Full Pipeline
#
# Oracle → Architect → Sentient → Workers → VOTE
#
# Usage:
#   bash demos/architect_demo.sh --llm=http://localhost:8082

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

LLM_URL=""
for arg in "$@"; do
    case $arg in
        --llm=*) LLM_URL="${arg#*=}" ;;
    esac
done

if [ -z "$LLM_URL" ]; then
    echo "Usage: bash demos/architect_demo.sh --llm=http://localhost:8082"
    echo ""
    echo "The Architect requires an NML LLM server to generate programs."
    echo "Start one with: python3 serve/nml_server.py --http --port 8082 --model <path>"
    exit 1
fi

echo "═══════════════════════════════════════════════════════"
echo "  NML Architect Demo — Oracle → Architect → Sentient → Workers"
echo "═══════════════════════════════════════════════════════"
echo "  LLM: $LLM_URL"

nml_start sentient prime     9001 --data demos/agent1.nml.data --no-mdns; sleep 2
nml_start worker   worker_us 9002 --seeds 9001 --data demos/agent2.nml.data --no-mdns
nml_start worker   worker_eu 9003 --seeds 9001 --data demos/agent3.nml.data --no-mdns; sleep 2
nml_start oracle   sibyl     9004 --seeds 9001,9002,9003 --llm "$LLM_URL" --no-mdns; sleep 2
nml_start architect daedalus 9005 --seeds 9001,9004 --llm "$LLM_URL" --no-mdns; sleep 3

nml_check_mesh 9001 9002 9003 9004 9005

echo ""
echo "━━━ Step 1: Oracle generates program spec ━━━"
SPEC=$(curl -s -X POST http://localhost:9004/spec \
    -H "Content-Type: application/json" \
    -d '{"intent": "Detect fraudulent credit card transactions using 6 features"}' 2>/dev/null)
echo "$SPEC" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f'  Intent:       {d.get(\"intent\",\"?\")}')
print(f'  Architecture: {d.get(\"architecture\",\"?\")}')
print(f'  Features:     {d.get(\"features\",\"?\")}')
print(f'  Output:       {d.get(\"output_syntax\",\"?\")}')
" 2>/dev/null

echo ""
echo "━━━ Step 2: Architect builds symbolic NML ━━━"
BUILD=$(curl -s -X POST http://localhost:9005/build \
    -H "Content-Type: application/json" \
    -d "$SPEC" 2>/dev/null)
echo "$BUILD" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f'  Status:   {d.get(\"status\",\"?\")}')
print(f'  Hash:     {d.get(\"hash\",\"?\")}')
print(f'  Size:     {d.get(\"byte_size\",\"?\")} bytes  fits_udp={d.get(\"fits_udp\",\"?\")}')
v = d.get('validation',{})
print(f'  Valid:    {v.get(\"valid\",\"?\")}')
for e in v.get('errors',[]): print(f'    Error: {e}')
" 2>/dev/null

echo ""
echo "━━━ Step 3: Submit to collective ━━━"
PROGRAM=$(echo "$BUILD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('program',''))" 2>/dev/null)
if [ -n "$PROGRAM" ]; then
    curl -s -X POST http://localhost:9001/submit \
        -H "Content-Type: application/json" \
        -d "{\"program\": $(echo "$PROGRAM" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))')}" \
        | python3 -c "import sys,json; print('  Submitted — hash: ' + json.load(sys.stdin).get('hash','?'))" 2>/dev/null
    echo "  Waiting for execution..."
    sleep 6
fi

echo ""
echo "━━━ Step 4: VOTE Consensus ━━━"
curl -s -X POST http://localhost:9004/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy":"median"}' 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
if d.get('consensus') is not None:
    print(f'  Strategy:   {d[\"strategy\"]}')
    print(f'  Consensus:  {d[\"consensus\"]:.4f}')
    print(f'  Agents:     {d[\"count\"]}')
    a = d.get('assessment',{})
    if a: print(f'  Confidence: {a[\"confidence\"]} — {a[\"confidence_reason\"]}')
else:
    print(f'  {d.get(\"error\",\"No results\")}')
" 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Pipeline complete: Oracle → Architect → Sentient → Workers → VOTE"
echo ""
echo "  Dashboards:"
echo "    Sentient:  http://localhost:9001/dashboard"
echo "    Oracle:    http://localhost:9004/dashboard"
echo "    Architect: http://localhost:9005/dashboard"
echo ""
echo "  Build another program:"
echo "    curl -X POST http://localhost:9005/build \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"intent\": \"anomaly detection on sensor data\", \"features\": 4}'"
echo ""
echo "  Add more workers:"
echo "    bash demos/agent.sh worker --name w_asia --port 9007 --seeds 9001 --no-mdns"
echo ""
echo "  Press Ctrl+C to stop all agents."
echo "═══════════════════════════════════════════════════════"

wait
