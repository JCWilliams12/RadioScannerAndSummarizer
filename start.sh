#!/usr/bin/env bash
# =============================================================================
# start.sh — One-line launcher for AetherGuard
# =============================================================================
# Usage:  ./start.sh          (start everything)
#         ./start.sh stop     (stop everything)
#         ./start.sh relay    (start only the native relay)
#
# This script detects the host OS and:
#   - On Windows/macOS: starts sdr-relay natively, then docker compose up
#   - On Linux with USB: optionally runs relay natively or uses Docker --device
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_DIR="$SCRIPT_DIR/sdr-relay"
RELAY_BIN="$RELAY_DIR/build/sdr_relay"
RELAY_PID_FILE="$SCRIPT_DIR/.sdr-relay.pid"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ---- Detect OS ----
detect_os() {
    case "$(uname -s)" in
        Linux*)   echo "linux" ;;
        Darwin*)  echo "macos" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *)        echo "unknown" ;;
    esac
}

# ---- Build relay if not already built ----
build_relay() {
    if [[ -x "$RELAY_BIN" ]]; then
        echo -e "${GREEN}[✓] Relay binary found.${NC}"
        return 0
    fi

    echo -e "${YELLOW}[…] Building sdr-relay...${NC}"
    mkdir -p "$RELAY_DIR/build"
    pushd "$RELAY_DIR/build" > /dev/null
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    popd > /dev/null

    if [[ ! -x "$RELAY_BIN" ]]; then
        echo -e "${RED}[✗] Relay build failed.${NC}"
        exit 1
    fi
    echo -e "${GREEN}[✓] Relay built successfully.${NC}"
}

# ---- Start relay as background process ----
start_relay() {
    if [[ -f "$RELAY_PID_FILE" ]]; then
        local old_pid
        old_pid=$(cat "$RELAY_PID_FILE")
        if kill -0 "$old_pid" 2>/dev/null; then
            echo -e "${CYAN}[i] Relay already running (PID $old_pid).${NC}"
            return 0
        fi
        rm -f "$RELAY_PID_FILE"
    fi

    echo -e "${YELLOW}[…] Starting sdr-relay...${NC}"
    "$RELAY_BIN" &
    local pid=$!
    echo "$pid" > "$RELAY_PID_FILE"

    # Give it a moment to bind ports
    sleep 2

    if kill -0 "$pid" 2>/dev/null; then
        echo -e "${GREEN}[✓] Relay running (PID $pid) — ports 7373/7374${NC}"
    else
        echo -e "${RED}[✗] Relay exited immediately. Check SDR hardware and API service.${NC}"
        rm -f "$RELAY_PID_FILE"
        exit 1
    fi
}

# ---- Stop relay ----
stop_relay() {
    if [[ -f "$RELAY_PID_FILE" ]]; then
        local pid
        pid=$(cat "$RELAY_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${YELLOW}[…] Stopping relay (PID $pid)...${NC}"
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
        rm -f "$RELAY_PID_FILE"
    fi
}

# ---- Main ----
main() {
    local cmd="${1:-start}"
    local os
    os=$(detect_os)

    echo ""
    echo -e "${CYAN}╔══════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║       AetherGuard — Launcher             ║${NC}"
    echo -e "${CYAN}║  Detected OS: $(printf '%-27s' "$os")║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════╝${NC}"
    echo ""

    case "$cmd" in
        stop)
            stop_relay
            docker compose -f "$SCRIPT_DIR/docker-compose.yml" down
            echo -e "${GREEN}[✓] Everything stopped.${NC}"
            ;;

        relay)
            build_relay
            start_relay
            ;;

        start|up)
            case "$os" in
                linux)
                    # On Linux, check if USB devices are accessible
                    if [[ -d /dev/bus/usb ]]; then
                        echo -e "${CYAN}[i] Linux with USB access detected.${NC}"
                        echo -e "${CYAN}[i] Starting relay natively for best performance.${NC}"
                        echo -e "${CYAN}[i] (To use Docker USB passthrough instead, set SDR_RELAY_HOST=ag-sdr-relay)${NC}"
                        export SDR_RELAY_HOST="172.17.0.1"  # Docker bridge gateway
                    else
                        echo -e "${YELLOW}[!] No /dev/bus/usb — relay must be running on a remote host.${NC}"
                        echo -e "${YELLOW}    Set SDR_RELAY_HOST to the relay's IP address.${NC}"
                    fi
                    ;;

                macos)
                    export SDR_RELAY_HOST="host.docker.internal"
                    echo -e "${CYAN}[i] macOS: using host.docker.internal for relay connection.${NC}"
                    ;;

                windows)
                    export SDR_RELAY_HOST="host.docker.internal"
                    echo -e "${CYAN}[i] Windows: using host.docker.internal for relay connection.${NC}"
                    echo -e "${GREEN}[i] Native USB — full interrupt rate, no WSL2 bottleneck!${NC}"
                    ;;

                *)
                    echo -e "${YELLOW}[!] Unknown OS. Defaulting SDR_RELAY_HOST=host.docker.internal${NC}"
                    export SDR_RELAY_HOST="host.docker.internal"
                    ;;
            esac

            # Build and start relay
            build_relay
            start_relay

            # Start Docker pipeline
            echo ""
            echo -e "${YELLOW}[…] Starting Docker pipeline...${NC}"
            docker compose -f "$SCRIPT_DIR/docker-compose.yml" up --build

            # When docker compose exits (Ctrl+C), clean up relay
            echo ""
            echo -e "${YELLOW}[…] Docker stopped. Cleaning up...${NC}"
            stop_relay
            echo -e "${GREEN}[✓] Done.${NC}"
            ;;

        *)
            echo "Usage: $0 [start|stop|relay]"
            exit 1
            ;;
    esac
}

main "$@"