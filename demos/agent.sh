#!/bin/bash
# Start a single NML collective agent.
#
# Usage: bash demos/agent.sh <role> [--name NAME] [--port PORT] [options]
#
# Roles: sentient | worker | oracle | architect | enforcer
#
# Options:
#   --name NAME     Agent name (default: <role>_<port>)
#   --port PORT     HTTP port  (default: 9001)
#   --seeds SEEDS   Comma-separated ports or URLs
#                     9001,9002          → http://localhost:9001,...
#                     http://host:9001   → used as-is
#   --data FILE     Local .nml.data file
#   --llm  URL      LLM server URL
#   --key  HEX      HMAC signing key
#   --no-mdns       Disable mDNS discovery
#   --no-udp        Disable UDP multicast
#
# Examples:
#   bash demos/agent.sh sentient --port 9001
#   bash demos/agent.sh worker   --name w_us --port 9002 --seeds 9001 --data demos/agent1.nml.data
#   bash demos/agent.sh worker   --name w_eu --port 9003 --seeds 9001 --data demos/agent2.nml.data
#   bash demos/agent.sh oracle   --name sibyl    --port 9004 --seeds 9001,9002,9003
#   bash demos/agent.sh enforcer --name guardian --port 9006 --seeds 9001
#   bash demos/agent.sh architect --name daedalus --port 9005 --seeds 9001,9004 --llm http://localhost:8082

set -e
cd "$(dirname "$0")/.."

ROLE="${1:?Usage: bash demos/agent.sh <role> [--name NAME] [--port PORT] [...]}"
shift

NAME="" PORT="" SEEDS="" DATA="" LLM="" PASSTHROUGH=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --name)    NAME="$2";  shift 2 ;;
        --port)    PORT="$2";  shift 2 ;;
        --seeds)   SEEDS="$2"; shift 2 ;;
        --data)    DATA="$2";  shift 2 ;;
        --llm)     LLM="$2";   shift 2 ;;
        --key)     PASSTHROUGH+=(--key "$2"); shift 2 ;;
        --no-mdns|--no-udp) PASSTHROUGH+=("$1"); shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

PORT="${PORT:-9001}"
NAME="${NAME:-${ROLE}_${PORT}}"

# Normalize seeds: bare port numbers → http://localhost:<port>
if [[ -n "$SEEDS" ]]; then
    NORMALIZED=""
    IFS=',' read -ra _PARTS <<< "$SEEDS"
    for _P in "${_PARTS[@]}"; do
        _P="${_P// /}"
        [[ "$_P" =~ ^[0-9]+$ ]] && _P="http://localhost:$_P"
        NORMALIZED="${NORMALIZED:+$NORMALIZED,}$_P"
    done
    SEEDS="$NORMALIZED"
fi

ARGS=(--name "$NAME" --port "$PORT" --role "$ROLE")
[[ -n "$SEEDS" ]] && ARGS+=(--seeds "$SEEDS")
[[ -n "$DATA"  ]] && ARGS+=(--data "$DATA")
[[ -n "$LLM"   ]] && ARGS+=(--llm "$LLM")
ARGS+=("${PASSTHROUGH[@]}")

echo "[$NAME] Starting $ROLE on :$PORT"
exec python3 serve/nml_collective.py "${ARGS[@]}"
