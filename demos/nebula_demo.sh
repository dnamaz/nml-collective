#!/bin/bash
# NML Nebula Demo — Sentient + Workers + Quarantine
#
# 1. Sentient starts with embedded nebula (authority)
# 2. Workers submit data → quarantine
# 3. Sentient approves → data pool
# 4. Broadcast program → all agents train + classify
# 5. VOTE consensus
#
# Usage:
#   bash demos/nebula_demo.sh

set -e
cd "$(dirname "$0")/.."
# shellcheck source=demos/_lib.sh
source demos/_lib.sh
trap nml_cleanup EXIT INT TERM

echo "═══════════════════════════════════════════════════════"
echo "  NML Nebula Demo"
echo "═══════════════════════════════════════════════════════"

nml_start sentient oracle 9001 --data demos/agent1.nml.data --no-mdns; sleep 3

for i in 1 2 3; do
    nml_start worker "worker_$i" $((9001 + i)) --data "demos/agent${i}.nml.data" --no-mdns; sleep 2
done
sleep 3

echo ""
echo "━━━ Phase 1: Workers submit data to quarantine ━━━"
for i in 1 2 3; do
    REGIONS=("us_east" "europe" "asia")
    curl -s -X POST http://localhost:9001/data/submit \
        -H "Content-Type: application/json" \
        -d "{\"name\":\"transactions_${REGIONS[$((i-1))]}\",\"content\":\"@transactions shape=5,6 data=0.12,0.85,0.03,0.15,0.0,0.10,0.25,0.50,0.08,0.25,0.0,0.15,0.08,0.95,0.01,0.10,0.0,0.05,0.30,0.40,0.12,0.35,0.0,0.20,0.18,0.60,0.05,0.20,1.0,0.15\",\"author\":\"worker_$i\"}" \
        | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  worker_$i submitted: hash={d[\"hash\"]} status={d[\"status\"]} auto={d[\"auto_checks\"][\"passed\"]}')" 2>/dev/null
done

echo ""
echo "━━━ Phase 2: Quarantine status ━━━"
PENDING=$(curl -s http://localhost:9001/data/quarantine 2>/dev/null)
echo "$PENDING" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  {len(d[\"pending\"])} items in quarantine')
for p in d['pending']: print(f'    {p[\"hash\"]} by {p[\"author\"]} — {p[\"meta\"].get(\"name\",\"?\")}')
" 2>/dev/null

echo ""
echo "━━━ Phase 3: Sentient approves all ━━━"
echo "$PENDING" | python3 -c "
import sys,json
for p in json.load(sys.stdin)['pending']: print(p['hash'])" 2>/dev/null | while read HASH; do
    curl -s -X POST http://localhost:9001/data/approve \
        -H "Content-Type: application/json" \
        -d "{\"hash\":\"$HASH\",\"reason\":\"schema valid, distribution normal\"}" \
        | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  Approved: {d[\"hash\"]} → {d[\"status\"]}')" 2>/dev/null
done

echo ""
echo "━━━ Phase 4: Data pool ━━━"
curl -s http://localhost:9001/data/pool 2>/dev/null | python3 -c "
import sys,json
for name, versions in json.load(sys.stdin)['pool'].items():
    print(f'  @{name}: {len(versions)} version(s)')" 2>/dev/null

echo ""
echo "━━━ Phase 5: Broadcast fraud detection program ━━━"
nml_submit_program 9001 demos/fraud_detection.nml
sleep 4

echo ""
echo "━━━ Phase 6: Execution results ━━━"
for port in 9001 9002 9003 9004; do
    curl -s "http://localhost:$port/results" 2>/dev/null | python3 -c "
import sys,json
for r in json.load(sys.stdin)['results']:
    if 'score' in r: print(f'  {r[\"agent\"]:>10}  score={r[\"score\"]:.4f}')" 2>/dev/null
done

echo ""
echo "━━━ Phase 7: VOTE consensus ━━━"
curl -s -X POST http://localhost:9001/consensus \
    -H "Content-Type: application/json" \
    -d '{"strategy":"median"}' 2>/dev/null | python3 -c "
import sys,json; d=json.load(sys.stdin)
if 'consensus' in d:
    print(f'  Median: {d[\"consensus\"]:.4f} from {d[\"count\"]} agents')
    for a in d['agents']: print(f'    {a[\"agent\"]}: {a[\"score\"]:.4f}')
" 2>/dev/null

echo ""
echo "━━━ Nebula stats ━━━"
curl -s http://localhost:9001/nebula/stats 2>/dev/null | python3 -m json.tool

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Dashboard: http://localhost:9001/dashboard"
echo "  Nebula:    http://localhost:9001/nebula/stats"
echo ""
echo "  Add more workers:"
echo "    bash demos/agent.sh worker --name w_4 --port 9005 --no-mdns"
echo ""
echo "  Press Ctrl+C to stop."
echo "═══════════════════════════════════════════════════════"

wait
