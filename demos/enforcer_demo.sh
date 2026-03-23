#!/bin/bash
# NML Enforcer Demo — The Collective's Immune System
#
# 1. Normal collective operation with enforcer monitoring
# 2. Enforcer quarantines a node (simulated threat)
# 3. Quarantined node excluded from VOTE
# 4. Quarantine lifted
#
# Usage:
#   bash demos/enforcer_demo.sh

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

echo "═══════════════════════════════════════════════════════"
echo "  NML Enforcer Demo — Immune System"
echo "═══════════════════════════════════════════════════════"

nml_start sentient  prime     9001 --data demos/agent1.nml.data --no-mdns; sleep 2
nml_start worker    worker_us 9002 --seeds 9001 --data demos/agent2.nml.data --no-mdns
nml_start worker    worker_eu 9003 --seeds 9001 --data demos/agent3.nml.data --no-mdns; sleep 2
nml_start enforcer  guardian  9006 --seeds 9001 --no-mdns; sleep 3

nml_check_mesh 9001 9002 9003 9006

echo ""
echo "━━━ Broadcasting fraud detection program ━━━"
nml_submit_program 9001 demos/fraud_detection.nml
sleep 5

echo ""
echo "━━━ Threat Board (should be clean) ━━━"
curl -s http://localhost:9006/threats | python3 -c "
import sys,json
d=json.load(sys.stdin)
w=len(d.get('warnings',{})); q=len(d.get('quarantined',{})); b=len(d.get('blacklisted',{}))
print(f'  Warnings: {w}, Quarantined: {q}, Blacklisted: {b}')
if w==0 and q==0 and b==0: print('  All clear.')" 2>/dev/null

echo ""
echo "━━━ Quarantining worker_eu (simulated threat) ━━━"
curl -s -X POST http://localhost:9006/quarantine/node \
    -H "Content-Type: application/json" \
    -d '{"agent": "worker_eu", "reason": "Simulated: suspicious score pattern"}' \
    | python3 -m json.tool 2>/dev/null

echo ""
echo "━━━ Threat Board (after quarantine) ━━━"
curl -s http://localhost:9006/threats | python3 -c "
import sys,json
for name,q in json.load(sys.stdin).get('quarantined',{}).items():
    print(f'  {name}: QUARANTINED — {q[\"reason\"]} ({q[\"remaining_seconds\"]}s left)')" 2>/dev/null

echo ""
echo "━━━ VOTE Consensus (worker_eu excluded) ━━━"
curl -s -X POST http://localhost:9006/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy":"median"}' | python3 -c "
import sys,json
d=json.load(sys.stdin)
if d.get('consensus') is not None:
    agents=[a['agent'] for a in d.get('agents',[])]
    print(f'  Consensus: {d[\"consensus\"]:.4f} ({d[\"count\"]} agents: {\", \".join(agents)})')
    if 'worker_eu' not in agents: print('  worker_eu successfully excluded from VOTE')
else:
    print(f'  {d.get(\"error\",\"?\")}')
" 2>/dev/null

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
echo "  Add more agents:"
echo "    bash demos/agent.sh worker --name w_asia --port 9007 --seeds 9001 --no-mdns"
echo ""
echo "  Press Ctrl+C to stop."
echo "═══════════════════════════════════════════════════════"

wait
