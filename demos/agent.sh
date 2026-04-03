#!/bin/bash
# Start a single NML collective agent (C99 binary).
#
# Usage: bash demos/agent.sh <role> [--name NAME] [--port PORT] [options]
#
# Roles: herald | sentient | worker | oracle | architect | enforcer
#        emissary | custodian
#
# Options:
#   --name NAME          Agent name (default: <role>_<port>)
#   --port PORT          HTTP API port  (default: 9001)
#   --broker HOST        MQTT broker host (default: 127.0.0.1)
#   --broker-port PORT   MQTT broker port (default: 1883)
#   --data FILE          Local .nml.data file (worker / sentient)
#   --data-name NAME     Fetch data by name from sentient (worker)
#   --llm-host URL       LLM server for program generation (architect)
#   --require-signed     Reject unsigned programs (worker)
#   --api-key KEY        Bearer token for external API (emissary)
#   --stale-after SEC    Data staleness threshold (custodian)
#
# Examples:
#   bash demos/agent.sh herald
#   bash demos/agent.sh sentient --name prime    --port 9001
#   bash demos/agent.sh worker   --name w_us     --port 9002 --data demos/agent1.nml.data
#   bash demos/agent.sh worker   --name w_eu     --port 9003 --data-name transactions
#   bash demos/agent.sh oracle   --name sibyl    --port 9004
#   bash demos/agent.sh architect --name daedalus --port 9005
#   bash demos/agent.sh enforcer --name guardian --port 9006
#   bash demos/agent.sh custodian --name vault   --port 9010
#   bash demos/agent.sh emissary  --name gateway --port 8080 --api-key secret

set -e
cd "$(dirname "$0")/.."

ROLE="${1:?Usage: bash demos/agent.sh <role> [--name NAME] [--port PORT] [...]}"
shift

NAME="" PORT="" BROKER_HOST="127.0.0.1" BROKER_PORT="1883" PASSTHROUGH=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --name)         NAME="$2";         shift 2 ;;
        --port)         PORT="$2";         shift 2 ;;
        --broker)       BROKER_HOST="$2";  shift 2 ;;
        --broker-port)  BROKER_PORT="$2";  shift 2 ;;
        *)              PASSTHROUGH+=("$1"); shift ;;
    esac
done

PORT="${PORT:-9001}"
NAME="${NAME:-${ROLE}_${PORT}}"

# Platform binary extension (.exe on Windows/MSYS/MinGW, empty on POSIX)
_EXT=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*) _EXT=".exe" ;;
esac

BIN="roles/$ROLE/${ROLE}_agent${_EXT}"
if [[ ! -f "$BIN" ]]; then
    echo "[BUILD] Building $ROLE..."
    make -C "roles/$ROLE" 2>&1 | tail -3
fi

if [[ "$ROLE" == "herald" ]]; then
    # Herald IS the broker — no --broker flags
    echo "[$NAME] Starting herald (MQTT broker) on :${BROKER_PORT}"
    exec "$BIN" --broker-port "$BROKER_PORT" --api-port "$PORT" "${PASSTHROUGH[@]}"
else
    ARGS=(--name "$NAME" --port "$PORT"
          --broker "$BROKER_HOST" --broker-port "$BROKER_PORT"
          "${PASSTHROUGH[@]}")
    echo "[$NAME] Starting $ROLE on http://localhost:$PORT  broker=${BROKER_HOST}:${BROKER_PORT}"
    exec "$BIN" "${ARGS[@]}"
fi
