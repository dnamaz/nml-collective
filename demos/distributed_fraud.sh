#!/bin/bash
# ═══════════════════════════════════════════════════════════
# NML M2M Demo: Distributed Fraud Detection
# ═══════════════════════════════════════════════════════════
# Demonstrates the full M2M lifecycle:
#   1. Sign a fraud detection program
#   2. Start a hub + 3 agents (each with local data)
#   3. Distribute the signed program to all agents
#   4. Each agent trains locally (TNET) and classifies
#   5. Hub collects scores and applies VOTE consensus
#   6. Apply a PTCH to update threshold, re-sign, re-distribute
# ═══════════════════════════════════════════════════════════

set -e
cd "$(dirname "$0")/.."

NML=./nml-crypto
KEY="deadbeef01020304050607080910111213141516"
AGENT="fraud_authority_v1"

echo "═══════════════════════════════════════════════════════"
echo "  NML M2M Demo: Distributed Fraud Detection"
echo "═══════════════════════════════════════════════════════"

# Build if needed
if [ ! -f "$NML" ]; then
    echo ""
    echo "[BUILD] Building nml-crypto..."
    make nml-crypto
fi

echo ""
echo "━━━ Phase 1: Sign the program ━━━"
$NML --sign programs/fraud_detection.nml --key $KEY --agent $AGENT > /tmp/m2m_signed.nml
echo "  Signed by: $AGENT"
head -1 /tmp/m2m_signed.nml | cut -c1-80
echo "  ..."

echo ""
echo "━━━ Phase 2: Distribute to 3 agents (local execution) ━━━"

SCORES=()
for i in 1 2 3; do
    echo ""
    echo "  --- Agent $i ---"
    RESULT=$($NML /tmp/m2m_signed.nml demos/agent${i}.nml.data --max-cycles 1000000 2>&1)

    SCORE=$(echo "$RESULT" | grep "fraud_score" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
    DECISION=$(echo "$RESULT" | grep "decision" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
    LOSS=$(echo "$RESULT" | grep "training_loss" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
    VERIFIED=$(echo "$RESULT" | grep "Signature verified" | head -1)

    echo "  $VERIFIED"
    echo "  Training loss: $LOSS"
    echo "  Fraud score:   $SCORE"
    echo "  Decision:      $DECISION ($([ "$DECISION" = "1.0000" ] && echo "FLAGGED" || echo "APPROVED"))"
    SCORES+=("$SCORE")
done

echo ""
echo "━━━ Phase 3: VOTE consensus (median) ━━━"

# Sort and find median
IFS=$'\n' SORTED=($(sort -n <<<"${SCORES[*]}")); unset IFS
MEDIAN=${SORTED[1]}
echo "  Agent scores: ${SCORES[0]}, ${SCORES[1]}, ${SCORES[2]}"
echo "  Median (VOTE): $MEDIAN"

# Determine consensus
CONSENSUS=$(python3 -c "print('FRAUD' if float('$MEDIAN') >= 0.5 else 'LEGITIMATE')")
echo "  Consensus decision: $CONSENSUS"

echo ""
echo "━━━ Phase 4: PTCH — update threshold to 0.6 ━━━"

cat > /tmp/m2m_patch.ptch <<'PATCH'
PTCH @base sha256:TBD
PTCH @set 49 CMPI  RE RA #0.6
PTCH @end
PATCH

$NML programs/fraud_detection.nml --patch /tmp/m2m_patch.ptch > /tmp/m2m_patched.nml
echo "  Threshold updated: 0.5 → 0.6"

echo ""
echo "━━━ Phase 5: Re-sign and re-distribute ━━━"

$NML --sign /tmp/m2m_patched.nml --key $KEY --agent "${AGENT}_patch1" > /tmp/m2m_signed_v2.nml
echo "  Re-signed by: ${AGENT}_patch1"

SCORES2=()
for i in 1 2 3; do
    RESULT=$($NML /tmp/m2m_signed_v2.nml demos/agent${i}.nml.data --max-cycles 1000000 2>&1)
    SCORE=$(echo "$RESULT" | grep "fraud_score" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
    DECISION=$(echo "$RESULT" | grep "decision" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
    echo "  Agent $i: score=$SCORE decision=$DECISION ($([ "$DECISION" = "1.0000" ] && echo "FLAGGED" || echo "APPROVED"))"
    SCORES2+=("$SCORE")
done

IFS=$'\n' SORTED2=($(sort -n <<<"${SCORES2[*]}")); unset IFS
MEDIAN2=${SORTED2[1]}
CONSENSUS2=$(python3 -c "print('FRAUD' if float('$MEDIAN2') >= 0.6 else 'LEGITIMATE')")
echo ""
echo "  Updated consensus (threshold=0.6): $CONSENSUS2"

echo ""
echo "━━━ Phase 6: Tamper test ━━━"

sed 's/TNET  #1000/TNET  #9999/' /tmp/m2m_signed.nml > /tmp/m2m_tampered.nml
TAMPER_RESULT=$($NML /tmp/m2m_tampered.nml demos/agent1.nml.data 2>&1 || true)
if echo "$TAMPER_RESULT" | grep -q "SIGNATURE VERIFICATION FAILED"; then
    echo "  Tampered program REJECTED — signature verification failed"
else
    echo "  WARNING: Tampered program was not rejected!"
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  Demo complete."
echo "  Program: 23 instructions, 83KB runtime, signed HMAC-SHA256"
echo "  Agents: 3 regions, each trained locally with TNET"
echo "  Consensus: VOTE median across agent scores"
echo "  Update: PTCH threshold change, re-signed, re-distributed"
echo "═══════════════════════════════════════════════════════"
