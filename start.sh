#!/usr/bin/env bash
# =============================================================================
# start.sh — One-line launcher for AetherGuard (Linux / macOS)
# =============================================================================
#
# Usage:
#   ./start.sh              Start everything (relay + Docker + frontend)
#   ./start.sh stop         Stop everything
#   ./start.sh relay        Start only the native relay
#   ./start.sh docker       Start only the Docker pipeline
#   ./start.sh frontend     Start only the frontend dev server
#   ./start.sh build        Build/rebuild the relay
#   ./start.sh status       Show what's running
#
# On a fresh install this script will:
#   1. Create sdr-relay/build and run cmake configure
#   2. Build the relay binary
#   3. Install npm packages in client/ if needed
#   4. Start all three processes
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_DIR="$SCRIPT_DIR/sdr-relay"
RELAY_BIN="$RELAY_DIR/build/sdr_relay"
RELAY_PID_FILE="$SCRIPT_DIR/.sdr-relay.pid"
RELAY_LOG="$SCRIPT_DIR/.sdr-relay.log"
CLIENT_DIR="$SCRIPT_DIR/client"
FRONTEND_PID_FILE="$SCRIPT_DIR/.frontend.pid"

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ---- Detect OS ----
detect_os() {
    case "$(uname -s)" in
        Linux*)   echo "linux" ;;
        Darwin*)  echo "macos" ;;
        *)        echo "unknown" ;;
    esac
}

# ---- CPU count ----
cpu_count() {
    nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4
}

# ---- Banner ----
print_banner() {
    local os="$1"
    echo ""
    echo -e "${CYAN}  ==================================================${NC}"
    echo -e "${CYAN}    ${BOLD}AetherGuard${NC}${CYAN} — Software Defined Radio${NC}"
    echo -e "${CYAN}    OS: $os${NC}"
    echo -e "${CYAN}  ==================================================${NC}"
    echo ""
}

# =============================================================================
# CHECK PREREQUISITES
# =============================================================================
check_prereqs() {
    echo -e "  ${YELLOW}[..]${NC} Checking prerequisites..."
    local missing=()

    command -v cmake &>/dev/null || missing+=("cmake")
    command -v docker &>/dev/null || missing+=("docker")
    command -v node &>/dev/null || missing+=("node")
    command -v npm &>/dev/null || missing+=("npm")

    if ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
        missing+=("g++ or clang++")
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo -e "  ${RED}[!!]${NC} Missing: ${missing[*]}"
        exit 1
    fi

    # Docker compose (v2 plugin or standalone)
    if ! docker compose version &>/dev/null 2>&1; then
        if ! command -v docker-compose &>/dev/null; then
            echo -e "  ${RED}[!!]${NC} Missing: docker compose"
            exit 1
        fi
    fi

    # SDRplay API
    if [[ ! -f /usr/local/lib/libsdrplay_api.so ]] && \
       [[ ! -f /usr/local/lib/libsdrplay_api.dylib ]] && \
       [[ ! -f /opt/sdrplay/lib/libsdrplay_api.so ]]; then
        echo -e "  ${YELLOW}[!!]${NC} SDRplay API not found — cmake may fail."
        echo -e "       Download from: https://www.sdrplay.com/downloads/"
    fi

    echo -e "  ${GREEN}[OK]${NC} Prerequisites satisfied."
    echo ""
}

# =============================================================================
# ENSURE SDRPLAY API SERVICE
# =============================================================================
ensure_sdr_service() {
    local os="$1"
    [[ "$os" != "linux" ]] && return 0

    if pgrep -x sdrplay_apiService &>/dev/null; then
        echo -e "  ${GREEN}[OK]${NC} SDRplay API service is running."
        return 0
    fi

    echo -e "  ${YELLOW}[..]${NC} Starting SDRplay API service..."

    if command -v systemctl &>/dev/null; then
        sudo systemctl start sdrplayService 2>/dev/null && {
            echo -e "  ${GREEN}[OK]${NC} SDRplay API service started."
            return 0
        }
    fi

    for svc in /opt/sdrplay_api/sdrplay_apiService /usr/local/bin/sdrplay_apiService; do
        if [[ -x "$svc" ]]; then
            sudo "$svc" &
            sleep 2
            if pgrep -x sdrplay_apiService &>/dev/null; then
                echo -e "  ${GREEN}[OK]${NC} SDRplay API service started."
                return 0
            fi
        fi
    done

    echo -e "  ${RED}[!!]${NC} Could not start SDRplay API service."
    return 1
}

# =============================================================================
# BUILD RELAY (fresh-install safe)
# =============================================================================
build_relay() {
    echo -e "  ${YELLOW}[..]${NC} Preparing sdr-relay build..."

    # Create build directory (fresh install)
    if [[ ! -d "$RELAY_DIR/build" ]]; then
        echo -e "  ${YELLOW}[..]${NC} Creating build directory..."
        mkdir -p "$RELAY_DIR/build"
    fi

    # Run cmake configure if no cache (fresh install)
    if [[ ! -f "$RELAY_DIR/build/CMakeCache.txt" ]]; then
        echo -e "  ${YELLOW}[..]${NC} Running cmake configure (first time setup)..."
        pushd "$RELAY_DIR/build" > /dev/null
        cmake .. -DCMAKE_BUILD_TYPE=Release
        popd > /dev/null
        echo -e "  ${GREEN}[OK]${NC} CMake configured."
    fi

    echo -e "  ${YELLOW}[..]${NC} Building sdr-relay..."
    pushd "$RELAY_DIR/build" > /dev/null
    make -j"$(cpu_count)" 2>&1 | tail -5
    popd > /dev/null

    if [[ ! -x "$RELAY_BIN" ]]; then
        echo -e "  ${RED}[!!]${NC} Build failed."
        return 1
    fi

    echo -e "  ${GREEN}[OK]${NC} Relay built: $RELAY_BIN"
    echo ""
}

# =============================================================================
# INSTALL FRONTEND DEPS (fresh-install safe)
# =============================================================================
install_frontend() {
    if [[ ! -d "$CLIENT_DIR/node_modules" ]]; then
        echo -e "  ${YELLOW}[..]${NC} Installing frontend dependencies (first time setup)..."
        pushd "$CLIENT_DIR" > /dev/null
        npm install
        popd > /dev/null
        echo -e "  ${GREEN}[OK]${NC} Frontend dependencies installed."
    else
        echo -e "  ${GREEN}[OK]${NC} Frontend dependencies present."
    fi
    echo ""
}

# =============================================================================
# DOCKER COMPOSE HELPER
# =============================================================================
dc() {
    if docker compose version &>/dev/null 2>&1; then
        docker compose "$@"
    else
        docker-compose "$@"
    fi
}

# =============================================================================
# START PROCESSES
# =============================================================================
start_relay() {
    if [[ -f "$RELAY_PID_FILE" ]]; then
        local old_pid
        old_pid=$(cat "$RELAY_PID_FILE")
        if kill -0 "$old_pid" 2>/dev/null; then
            echo -e "  ${GREEN}[OK]${NC} Relay already running (PID $old_pid)."
            return 0
        fi
        rm -f "$RELAY_PID_FILE"
    fi

    echo -e "  ${YELLOW}[..]${NC} Starting sdr-relay..."
    "$RELAY_BIN" > "$RELAY_LOG" 2>&1 &
    local pid=$!
    echo "$pid" > "$RELAY_PID_FILE"
    sleep 3

    if kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${GREEN}[OK]${NC} Relay running (PID $pid)"
        echo -e "       Data: 7373  |  Ctrl: 7374  |  Log: $RELAY_LOG"
    else
        echo -e "  ${RED}[!!]${NC} Relay exited. Last 15 lines:"
        tail -15 "$RELAY_LOG" 2>/dev/null || true
        rm -f "$RELAY_PID_FILE"
        return 1
    fi
    echo ""
}

start_docker() {
    local os="$1"
    case "$os" in
        linux)  export SDR_RELAY_HOST="${SDR_RELAY_HOST:-172.17.0.1}" ;;
        *)      export SDR_RELAY_HOST="${SDR_RELAY_HOST:-host.docker.internal}" ;;
    esac

    export SDR_MODE="${SDR_MODE:-live}"

    echo -e "  ${YELLOW}[..]${NC} Starting Docker pipeline (SDR_MODE=$SDR_MODE, SDR_RELAY_HOST=$SDR_RELAY_HOST)..."
    dc -f "$SCRIPT_DIR/docker-compose.yml" up --build -d
    echo -e "  ${GREEN}[OK]${NC} Docker containers starting in background."
    echo ""
}

start_frontend() {
    if [[ -f "$FRONTEND_PID_FILE" ]]; then
        local old_pid
        old_pid=$(cat "$FRONTEND_PID_FILE")
        if kill -0 "$old_pid" 2>/dev/null; then
            echo -e "  ${GREEN}[OK]${NC} Frontend already running (PID $old_pid)."
            return 0
        fi
        rm -f "$FRONTEND_PID_FILE"
    fi

    echo -e "  ${YELLOW}[..]${NC} Starting frontend dev server..."
    pushd "$CLIENT_DIR" > /dev/null
    npm run dev > "$SCRIPT_DIR/.frontend.log" 2>&1 &
    local pid=$!
    echo "$pid" > "$FRONTEND_PID_FILE"
    popd > /dev/null

    sleep 3
    if kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${GREEN}[OK]${NC} Frontend running (PID $pid)"
        local url
        url=$(grep -oE 'http://localhost:[0-9]+' "$SCRIPT_DIR/.frontend.log" 2>/dev/null | head -1 || true)
        [[ -n "$url" ]] && echo -e "       URL: $url"
        echo -e "       Log: $SCRIPT_DIR/.frontend.log"
    else
        echo -e "  ${RED}[!!]${NC} Frontend failed. Check $SCRIPT_DIR/.frontend.log"
    fi
    echo ""
}

# =============================================================================
# STOP PROCESSES
# =============================================================================
stop_relay() {
    if [[ -f "$RELAY_PID_FILE" ]]; then
        local pid
        pid=$(cat "$RELAY_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "  ${YELLOW}[..]${NC} Stopping relay (PID $pid)..."
            kill "$pid" 2>/dev/null || true
            for i in {1..10}; do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.5
            done
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
            echo -e "  ${GREEN}[OK]${NC} Relay stopped."
        fi
        rm -f "$RELAY_PID_FILE"
    else
        echo -e "  ${CYAN}[--]${NC} Relay was not running."
    fi
}

stop_docker() {
    echo -e "  ${YELLOW}[..]${NC} Stopping Docker containers..."
    dc -f "$SCRIPT_DIR/docker-compose.yml" down 2>/dev/null || true
    echo -e "  ${GREEN}[OK]${NC} Docker containers stopped."
}

stop_frontend() {
    if [[ -f "$FRONTEND_PID_FILE" ]]; then
        local pid
        pid=$(cat "$FRONTEND_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "  ${YELLOW}[..]${NC} Stopping frontend (PID $pid)..."
            kill "$pid" 2>/dev/null || true
            pkill -P "$pid" 2>/dev/null || true
            echo -e "  ${GREEN}[OK]${NC} Frontend stopped."
        fi
        rm -f "$FRONTEND_PID_FILE"
    else
        echo -e "  ${CYAN}[--]${NC} Frontend was not running."
    fi
}

# =============================================================================
# STATUS
# =============================================================================
show_status() {
    echo ""
    echo -e "  ${BOLD}=== AetherGuard Status ===${NC}"
    echo ""

    if [[ -f "$RELAY_PID_FILE" ]] && kill -0 "$(cat "$RELAY_PID_FILE")" 2>/dev/null; then
        echo -e "    Relay:          ${GREEN}RUNNING${NC} (PID $(cat "$RELAY_PID_FILE"))"
    else
        echo -e "    Relay:          ${RED}STOPPED${NC}"
    fi

    if pgrep -x sdrplay_apiService &>/dev/null; then
        echo -e "    SDRplay API:    ${GREEN}RUNNING${NC}"
    else
        echo -e "    SDRplay API:    ${YELLOW}NOT DETECTED${NC}"
    fi

    if [[ -f "$FRONTEND_PID_FILE" ]] && kill -0 "$(cat "$FRONTEND_PID_FILE")" 2>/dev/null; then
        echo -e "    Frontend:       ${GREEN}RUNNING${NC} (PID $(cat "$FRONTEND_PID_FILE"))"
    else
        echo -e "    Frontend:       ${RED}STOPPED${NC}"
    fi

    echo ""
    for c in ag-redis ag-api ag-ai ag-sdr-live; do
        if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${c}$"; then
            printf "    %-16s ${GREEN}RUNNING${NC}\n" "$c:"
        else
            printf "    %-16s ${RED}STOPPED${NC}\n" "$c:"
        fi
    done
    echo ""
}

# =============================================================================
# MAIN
# =============================================================================
main() {
    local cmd="${1:-start}"
    local os
    os=$(detect_os)

    print_banner "$os"

    case "$cmd" in
        start|up)
            check_prereqs
            install_frontend

            # Try to build and start the relay — fall back to mock on failure
            SDR_MODE="live"

            if ! build_relay 2>/dev/null; then
                echo -e "  ${YELLOW}[!!]${NC} Relay build failed — switching to MOCK MODE."
                echo -e "       No SDR hardware needed. All features work with simulated data."
                echo ""
                SDR_MODE="mock"
            elif ! ensure_sdr_service "$os" 2>/dev/null; then
                echo -e "  ${YELLOW}[!!]${NC} SDR service unavailable — switching to MOCK MODE."
                echo ""
                SDR_MODE="mock"
            elif ! start_relay 2>/dev/null; then
                echo -e "  ${YELLOW}[!!]${NC} Relay failed to start — switching to MOCK MODE."
                echo -e "       Check that SDR hardware is connected."
                echo ""
                SDR_MODE="mock"
            fi

            export SDR_MODE
            start_docker "$os"
            start_frontend

            echo -e "  ${CYAN}==================================================${NC}"
            echo -e "    ${BOLD}AetherGuard is running!${NC}"
            if [[ "$SDR_MODE" == "mock" ]]; then
                echo -e "    MODE: ${YELLOW}MOCK${NC} — no SDR hardware"
            else
                echo -e "    MODE: ${GREEN}LIVE${NC} — SDR relay active"
            fi
            echo ""
            echo -e "    Frontend:  Check log for dev server URL"
            echo -e "    API:       http://localhost:8080"
            if [[ "$SDR_MODE" == "live" ]]; then
                echo -e "    SDR Relay: localhost:7373 (data) / 7374 (ctrl)"
            fi
            echo ""
            echo -e "    To stop:   ./start.sh stop"
            echo -e "  ${CYAN}==================================================${NC}"
            echo ""

            if [[ "$SDR_MODE" == "live" ]]; then
                echo -e "  ${CYAN}[i]${NC} Tailing relay log (Ctrl+C to detach — services keep running)..."
                echo ""
                tail -f "$RELAY_LOG" 2>/dev/null || true
            else
                echo -e "  ${CYAN}[i]${NC} Following container logs (Ctrl+C to detach)..."
                echo ""
                dc -f "$SCRIPT_DIR/docker-compose.yml" logs -f 2>/dev/null || true
            fi
            ;;

        mock)
            check_prereqs
            install_frontend

            export SDR_MODE="mock"
            echo -e "  ${GREEN}[OK]${NC} Starting in MOCK MODE — no SDR hardware needed."
            echo ""

            start_docker "$os"
            start_frontend

            echo -e "  ${CYAN}==================================================${NC}"
            echo -e "    ${BOLD}AetherGuard is running in MOCK MODE!${NC}"
            echo ""
            echo -e "    Frontend:  Check log for dev server URL"
            echo -e "    API:       http://localhost:8080"
            echo -e "    SDR:       Simulated — 440 Hz tone, fake stations"
            echo ""
            echo -e "    To stop:   ./start.sh stop"
            echo -e "  ${CYAN}==================================================${NC}"
            echo ""
            echo -e "  ${CYAN}[i]${NC} Following container logs (Ctrl+C to detach)..."
            echo ""
            dc -f "$SCRIPT_DIR/docker-compose.yml" logs -f 2>/dev/null || true
            ;;

        stop|down)
            stop_frontend
            stop_docker
            stop_relay
            echo ""
            echo -e "  ${GREEN}[OK]${NC} Everything stopped."
            ;;

        relay)
            check_prereqs
            ensure_sdr_service "$os"
            build_relay
            start_relay
            echo -e "  ${CYAN}[i]${NC} Relay running. Start the rest with:"
            echo -e "       ./start.sh docker"
            echo -e "       ./start.sh frontend"
            ;;

        docker)
            start_docker "$os"
            echo -e "  ${CYAN}[i]${NC} Following container logs (Ctrl+C to detach)..."
            dc -f "$SCRIPT_DIR/docker-compose.yml" logs -f
            ;;

        frontend)
            install_frontend
            echo -e "  ${YELLOW}[..]${NC} Starting frontend dev server (foreground)..."
            cd "$CLIENT_DIR"
            exec npm run dev
            ;;

        build)
            check_prereqs
            build_relay
            ;;

        status)
            show_status
            ;;

        *)
            echo "  Usage: $0 {start|stop|mock|relay|docker|frontend|build|status}"
            echo ""
            echo "    start     Build relay, start relay + Docker + frontend (auto-fallback to mock)"
            echo "    stop      Stop everything"
            echo "    mock      Start in mock mode (no SDR hardware needed)"
            echo "    relay     Build and start only the native relay"
            echo "    docker    Start only the Docker pipeline"
            echo "    frontend  Start only the frontend dev server"
            echo "    build     Build/rebuild the relay binary"
            echo "    status    Show what's running"
            exit 1
            ;;
    esac
}

main "$@"