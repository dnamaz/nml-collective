# NML Collective demo helpers — C99 binaries + MQTT transport
#
# All agents connect to the Herald MQTT broker.
# Default broker: 127.0.0.1:1883 (override via BROKER_HOST / BROKER_PORT env vars)
#
# Provides:
#   PIDS, BROKER_HOST, BROKER_PORT
#   nml_cleanup                              Kill all tracked PIDs
#   nml_start_herald [--broker-port PORT]    Start Herald (Mosquitto supervisor)
#   nml_start <role> <name> <port> [opts]    Start any non-broker role
#   nml_check_build                          Build nml-crypto if missing
#   nml_check_agent <port> [label]           Print one health line for an HTTP role
#   nml_check_mesh <port...>                 Print status for each HTTP role port
#   nml_submit_program <sentient_port> <nml> POST program to sentient, print hash

_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_REPO_ROOT="$(cd "$_LIB_DIR/.." && pwd)"

PIDS=()
BROKER_HOST="${BROKER_HOST:-127.0.0.1}"
BROKER_PORT="${BROKER_PORT:-1883}"

nml_cleanup() {
    echo ""
    echo "Stopping collective..."
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    echo "Done."
}

# Start the Herald broker (Mosquitto supervisor).
# Herald IS the broker — it does not connect to one.
nml_start_herald() {
    local broker_port="${BROKER_PORT}"
    local http_port="9000"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --broker-port) broker_port="$2"; shift 2 ;;
            --http-port)   http_port="$2";   shift 2 ;;
            *) shift ;;
        esac
    done

    local bin="$_REPO_ROOT/roles/herald/herald_agent"
    if [[ ! -f "$bin" ]]; then
        echo "[BUILD] Building herald..."
        make -C "$_REPO_ROOT/roles/herald" 2>&1 | tail -3
    fi

    echo "[herald] Starting MQTT broker on :${broker_port}"
    "$bin" --broker-port "$broker_port" --http-port "$http_port" &
    PIDS+=($!)
    BROKER_PORT="$broker_port"
}

# Start any non-broker role.
# nml_start <role> <name> <port> [--data FILE] [--data-name NAME]
#                                [--llm-host URL] [--require-signed]
#                                [--stale-after SEC] [--api-key KEY]
#                                [passthrough flags...]
nml_start() {
    local role="$1" name="$2" port="$3"; shift 3
    local data="" data_name="" llm_host="" passthrough=()

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --data)         data="$2";      shift 2 ;;
            --data-name)    data_name="$2"; shift 2 ;;
            --llm-host)     llm_host="$2";  shift 2 ;;
            *)              passthrough+=("$1"); shift ;;
        esac
    done

    local bin="$_REPO_ROOT/roles/$role/${role}_agent"
    if [[ ! -f "$bin" ]]; then
        echo "[BUILD] Building $role..."
        make -C "$_REPO_ROOT/roles/$role" 2>&1 | tail -3
    fi

    local args=(--name "$name" --port "$port"
                --broker "$BROKER_HOST" --broker-port "$BROKER_PORT")
    [[ -n "$data"      ]] && args+=(--data "$data")
    [[ -n "$data_name" ]] && args+=(--data-name "$data_name")
    [[ -n "$llm_host"  ]] && args+=(--llm-host "$llm_host")
    args+=("${passthrough[@]}")

    echo "[$name] Starting $role on http://localhost:$port"
    "$bin" "${args[@]}" &
    PIDS+=($!)
}

nml_check_build() {
    # Build the shared edge library if not already built.
    # Role binaries are auto-built by nml_start if missing.
    if [[ ! -f "$_REPO_ROOT/edge/libcollective.a" ]]; then
        echo "[BUILD] Building edge library..."
        make -C "$_REPO_ROOT/edge" 2>&1 | tail -3
    fi
}

# Print one status line for an HTTP role (sentient, oracle, architect, custodian, emissary).
# Workers and enforcers have no HTTP API.
nml_check_agent() {
    local port="$1" label="${2:-:$1}"
    printf "  %s  " "$label"
    local resp agent peers
    resp=$(curl -s --max-time 2 "http://localhost:$port/health" 2>/dev/null) \
        || { echo "unreachable"; return; }
    agent=$(echo "$resp" | grep -o '"agent":"[^"]*"' | cut -d'"' -f4 2>/dev/null)
    peers=$(echo "$resp" | grep -o '"peers":[0-9]*' | cut -d':' -f2 2>/dev/null)
    if [[ -n "$agent" ]]; then
        printf "%-18s — %s peers\n" "$agent" "${peers:-?}"
    else
        echo "ok"
    fi
}

nml_check_mesh() {
    echo ""
    echo "━━━ Mesh Status (HTTP roles only) ━━━"
    for port in "$@"; do
        nml_check_agent "$port"
    done
}

# POST a raw .nml file to sentient's /program endpoint and print the hash.
nml_submit_program() {
    local port="$1" nml_file="$2"
    local resp hash
    resp=$(curl -s -X POST "http://localhost:$port/program" \
        -H "Content-Type: text/plain" \
        --data-binary "@$nml_file" 2>/dev/null)
    hash=$(echo "$resp" | grep -o '"hash":"[^"]*"' | cut -d'"' -f4 2>/dev/null)
    echo "  Submitted to :$port — hash: ${hash:-?}"
    echo "  Response: $resp" | head -1
}
