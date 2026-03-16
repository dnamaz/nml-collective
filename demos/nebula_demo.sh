#!/bin/bash
# ═══════════════════════════════════════════════════════════
# NML Nebula Demo — Sentient + Workers + Quarantine
# ═══════════════════════════════════════════════════════════
# 1. Start a sentient (authority with embedded nebula)
# 2. Start 3 workers (compute nodes)
# 3. Workers submit data → quarantine
# 4. Sentient approves → data pool
# 5. Broadcast program → all agents train + classify
# 6. VOTE consensus
# ═══════════════════════════════════════════════════════════

set -e
cd "$(dirname "$0")/.."

PIDS=()
cleanup() { echo ""; echo "Stopping..."; for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; echo "Done."; }
trap cleanup EXIT INT TERM

echo "═══════════════════════════════════════════════════════"
echo "  NML Nebula Demo"
echo "═══════════════════════════════════════════════════════"

# Start sentient (authority — has embedded nebula)
echo ""
echo "[SENTIENT] Starting oracle on :9001"
python3 serve/nml_collective.py --name oracle --port 9001 --role sentient --no-mdns --data demos/agent1.nml.data 2>/dev/null &
PIDS+=($!)
sleep 3

# Start workers
for i in 1 2 3; do
    PORT=$((9001 + i))
    echo "[WORKER] Starting worker_$i on :$PORT"
    python3 serve/nml_collective.py --name "worker_$i" --port $PORT --role worker --no-mdns --data "demos/agent${i}.nml.data" 2>/dev/null &
    PIDS+=($!)
    sleep 2
done
sleep 3

echo ""
echo "━━━ Phase 1: Workers submit data to quarantine ━━━"
for i in 1 2 3; do
    REGION=("us_east" "europe" "asia")
    curl -s -X POST http://localhost:9001/data/submit \
        -H "Content-Type: application/json" \
        -d "{\"name\":\"transactions_${REGION[$((i-1))]}\",\"content\":\"@transactions shape=5,6 data=0.12,0.85,0.03,0.15,0.0,0.10,0.25,0.50,0.08,0.25,0.0,0.15,0.08,0.95,0.01,0.10,0.0,0.05,0.30,0.40,0.12,0.35,0.0,0.20,0.18,0.60,0.05,0.20,1.0,0.15\",\"author\":\"worker_$i\"}" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  worker_{$i} submitted: hash={d[\"hash\"]} status={d[\"status\"]} auto={d[\"auto_checks\"][\"passed\"]}')" 2>/dev/null
done

echo ""
echo "━━━ Phase 2: Quarantine status ━━━"
PENDING=$(curl -s http://localhost:9001/data/quarantine 2>/dev/null)
echo "$PENDING" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  {len(d[\"pending\"])} items in quarantine')
for p in d['pending']:
    print(f'    {p[\"hash\"]} by {p[\"author\"]} — {p[\"meta\"].get(\"name\",\"?\")}')" 2>/dev/null

echo ""
echo "━━━ Phase 3: Sentient approves all ━━━"
echo "$PENDING" | python3 -c "
import sys,json
for p in json.load(sys.stdin)['pending']:
    print(p['hash'])" 2>/dev/null | while read HASH; do
    curl -s -X POST http://localhost:9001/data/approve \
        -H "Content-Type: application/json" \
        -d "{\"hash\":\"$HASH\",\"reason\":\"schema valid, distribution normal\"}" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  Approved: {d[\"hash\"]} → {d[\"status\"]}')" 2>/dev/null
done

echo ""
echo "━━━ Phase 4: Data pool ━━━"
curl -s http://localhost:9001/data/pool 2>/dev/null | python3 -c "
import sys,json; d=json.load(sys.stdin)
for name, versions in d['pool'].items():
    print(f'  @{name}: {len(versions)} version(s)')" 2>/dev/null

echo ""
echo "━━━ Phase 5: Broadcast fraud detection program ━━━"
PROGRAM=$(cat demos/fraud_detection.nml)
curl -s -X POST http://localhost:9001/submit \
    -H "Content-Type: application/json" \
    -d "{\"program\": $(echo "$PROGRAM" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))')}" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  Broadcast: hash={d[\"hash\"]}')" 2>/dev/null

sleep 4

echo ""
echo "━━━ Phase 6: Execution results ━━━"
for PORT in 9001 9002 9003 9004; do
    curl -s http://localhost:$PORT/results 2>/dev/null | python3 -c "
import sys,json
for r in json.load(sys.stdin)['results']:
    if 'score' in r: print(f'  {r[\"agent\"]:>10}  score={r[\"score\"]:.4f}')" 2>/dev/null
done

echo ""
echo "━━━ Phase 7: VOTE consensus ━━━"
curl -s -X POST http://localhost:9001/consensus \
    -H "Content-Type: application/json" -d '{"strategy":"median"}' 2>/dev/null | python3 -c "
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
echo "  Sentient (oracle) glows purple in the dashboard."
echo "  Workers orbit around the nebula."
echo "  Quarantine items make the nebula pulse brighter."
echo ""
echo "  Press Ctrl+C to stop."
echo "═══════════════════════════════════════════════════════"

wait
