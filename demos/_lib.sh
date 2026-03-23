# NML Collective demo helpers. Source this file; do not run directly.
#
# Provides:
#   PIDS                 Tracked background agent PIDs (managed by nml_start)
#   nml_cleanup          Kill all tracked agents — use as trap handler
#   nml_start <role> <name> <port> [--seeds S] [--data F] [--llm U] [flags...]
#   nml_check_build      Ensure nml-crypto binary exists, build if missing
#   nml_check_mesh <port...>            Print one status line per port
#   nml_submit_program <port> <file>    POST program and print hash

_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_REPO_ROOT="$(cd "$_LIB_DIR/.." && pwd)"

PIDS=()

nml_cleanup() {
    echo ""
    echo "Stopping collective..."
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    echo "Done."
}

nml_start() {
    # nml_start <role> <name> <port> [--seeds S] [--data F] [--llm U] [passthrough flags]
    #
    # --seeds accepts comma-separated port numbers or full URLs:
    #   --seeds 9001,9002          → http://localhost:9001,http://localhost:9002
    #   --seeds http://host:9001   → used as-is
    #
    # All unrecognised flags (e.g. --no-mdns, --no-udp, --key HEX) are passed through.
    local role="$1" name="$2" port="$3"; shift 3
    local seeds="" data="" llm="" passthrough=()

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --seeds) seeds="$2"; shift 2 ;;
            --data)  data="$2";  shift 2 ;;
            --llm)   llm="$2";   shift 2 ;;
            *)       passthrough+=("$1"); shift ;;
        esac
    done

    # Normalize bare port numbers to localhost URLs
    if [[ -n "$seeds" ]]; then
        local normalized="" _p
        IFS=',' read -ra _parts <<< "$seeds"
        for _p in "${_parts[@]}"; do
            _p="${_p// /}"
            [[ "$_p" =~ ^[0-9]+$ ]] && _p="http://localhost:$_p"
            normalized="${normalized:+$normalized,}$_p"
        done
        seeds="$normalized"
    fi

    local args=(--name "$name" --port "$port" --role "$role")
    [[ -n "$seeds" ]] && args+=(--seeds "$seeds")
    [[ -n "$data"  ]] && args+=(--data "$data")
    [[ -n "$llm"   ]] && args+=(--llm "$llm")
    args+=("${passthrough[@]}")

    echo "[$name] Starting $role on :$port"
    python3 "$_REPO_ROOT/serve/nml_collective.py" "${args[@]}" &
    PIDS+=($!)
}

nml_check_build() {
    if [ ! -f "$_REPO_ROOT/nml-crypto" ]; then
        echo "[BUILD] Building nml-crypto..."
        make -C "$_REPO_ROOT" nml-crypto 2>&1 | tail -1
    fi
}

nml_check_mesh() {
    echo ""
    echo "━━━ Mesh Status ━━━"
    for port in "$@"; do
        printf "  :%s  " "$port"
        curl -s "http://localhost:$port/health" 2>/dev/null | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(f'{d[\"agent\"]:15s} — {d[\"peers\"]} peers, {d[\"programs\"]} programs')
" 2>/dev/null || echo "unreachable"
    done
}

nml_submit_program() {
    # nml_submit_program <port> <nml_file>
    local port="$1" nml_file="$2"
    local hash
    hash=$(curl -s -X POST "http://localhost:$port/submit" \
        -H "Content-Type: application/json" \
        -d "{\"program\": $(python3 -c 'import sys,json; print(json.dumps(sys.stdin.read()))' < "$nml_file")}" \
        | python3 -c "import sys,json; print(json.load(sys.stdin).get('hash','?'))" 2>/dev/null)
    echo "  Submitted to :$port — hash: $hash"
}
