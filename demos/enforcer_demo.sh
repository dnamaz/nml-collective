#!/bin/bash
# ═══════════════════════════════════════════════════════════
# NML Enforcer Demo — The Collective's Immune System
# ═══════════════════════════════════════════════════════════
# Demonstrates:
#   1. Normal collective operation with enforcer monitoring
#   2. Bad actor submitting suspicious data
#   3. Enforcer detects and quarantines the bad actor
#   4. Quarantined node excluded from VOTE
#
# Usage:
#   bash demos/enforcer_demo.sh
# ═══════════════════════════════════════════════════════════

set -e
cd "$(dirname "$0")/.."

PIDS=()

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
echo "  NML Enforcer Demo — Immune System"
echo "═══════════════════════════════════════════════════════"

# Start Sentient
echo ""
echo "[SENTIENT] Starting prime on :9001"
python3 serve/nml_collective.py \
    --name prime --port 9001 --role sentient \
    --data demos/agent1.nml.data --no-mdns &
PIDS+=($!)
sleep 2

# Start Workers
echo "[WORKER] Starting worker_us on :9002"
python3 serve/nml_collective.py \
    --name worker_us --port 9002 --role worker \
    --seeds http://localhost:9001 \
    --data demos/agent2.nml.data --no-mdns &
PIDS+=($!)

echo "[WORKER] Starting worker_eu on :9003"
python3 serve/nml_collective.py \
    --name worker_eu --port 9003 --role worker \
    --seeds http://localhost:9001 \
    --data demos/agent3.nml.data --no-mdns &
PIDS+=($!)
sleep 2

# Start Enforcer
echo "[ENFORCER] Starting guardian on :9006"
python3 serve/nml_collective.py \
    --name guardian --port 9006 --role enforcer \
    --seeds http://localhost:9001 --no-mdns &
PIDS+=($!)
sleep 3

# Verify mesh
echo ""
echo "━━━ Mesh Status ━━━"
for port in 9001 9002 9003 9006; do
    echo -n "  :$port  "
    curl -s http://localhost:$port/health
    echo
done

# Submit a program and wait for execution
echo ""
echo "━━━ Broadcasting fraud detection program ━━━"
PROGRAM=$(cat demos/fraud_detection.nml)
curl -s -X POST http://localhost:9001/submit \
    -H "Content-Type: application/json" \
    -d "{\"program\": $(echo "$PROGRAM" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))')}" > /dev/null
echo "  Submitted, waiting for execution..."
sleep 5

# Check threat board — should be clean
echo ""
echo "━━━ Threat Board (should be clean) ━━━"
curl -s http://localhost:9006/threats | python3 -c "
import sys,json
d=json.load(sys.stdin)
w = len(d.get('warnings', {}))
q = len(d.get('quarantined', {}))
b = len(d.get('blacklisted', {}))
print(f'  Warnings: {w}, Quarantined: {q}, Blacklisted: {b}')
if w == 0 and q == 0 and b == 0:
    print('  All clear.')
" 2>/dev/null

# Enforcer manually quarantines a suspicious node
echo ""
echo "━━━ Quarantining worker_eu (simulated threat) ━━━"
curl -s -X POST http://localhost:9006/quarantine/node \
    -H "Content-Type: application/json" \
    -d '{"agent": "worker_eu", "reason": "Simulated: suspicious score pattern"}' | python3 -m json.tool 2>/dev/null

# Check threat board — should show quarantine
echo ""
echo "━━━ Threat Board (after quarantine) ━━━"
curl -s http://localhost:9006/threats | python3 -c "
import sys,json
d=json.load(sys.stdin)
for name, q in d.get('quarantined', {}).items():
    print(f'  {name}: QUARANTINED — {q[\"reason\"]} ({q[\"remaining_seconds\"]}s left)')
" 2>/dev/null

# VOTE should now exclude worker_eu
echo ""
echo "━━━ VOTE Consensus (worker_eu excluded) ━━━"
curl -s -X POST http://localhost:9006/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy":"median"}' | python3 -c "
import sys,json
d=json.load(sys.stdin)
if d.get('consensus') is not None:
    agents = [a['agent'] for a in d.get('agents', [])]
    print(f'  Consensus: {d[\"consensus\"]:.4f} ({d[\"count\"]} agents: {\", \".join(agents)})')
    if 'worker_eu' not in agents:
        print('  worker_eu successfully excluded from VOTE')
else:
    print(f'  {d.get(\"error\", \"?\")}')" 2>/dev/null

# Lift the quarantine
echo ""
echo "━━━ Lifting quarantine on worker_eu ━━━"
curl -s -X POST http://localhost:9006/quarantine/lift \
    -H "Content-Type: application/json" \
    -d '{"agent": "worker_eu"}' | python3 -m json.tool 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Collective running with Enforcer."
echo ""
echo "  Dashboards:"
echo "    Sentient:  http://localhost:9001/dashboard"
echo "    Enforcer:  http://localhost:9006/dashboard"
echo ""
echo "  Enforcer API:"
echo "    curl http://localhost:9006/threats"
echo "    curl -X POST http://localhost:9006/quarantine/node \\"
echo "        -H 'Content-Type: application/json' \\"
echo "        -d '{\"agent\": \"worker_us\", \"reason\": \"test\"}'"
echo ""
echo "  Press Ctrl+C to stop."
echo "═══════════════════════════════════════════════════════"

wait
