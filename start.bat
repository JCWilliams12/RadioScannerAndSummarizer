@echo off
setlocal EnableDelayedExpansion
:: =============================================================================
:: start.bat — One-line launcher for AetherGuard (Windows)
:: =============================================================================
::
:: Usage:
::   start.bat              Start everything (auto-detects mock vs live)
::   start.bat mock         Start in mock mode (no SDR hardware needed)
::   start.bat stop         Stop everything
::   start.bat relay        Start only the native relay
::   start.bat docker       Start only the Docker pipeline
::   start.bat frontend     Start only the frontend dev server
::   start.bat build        Build/rebuild the relay
::   start.bat status       Show what's running
::
:: =============================================================================

set "SCRIPT_DIR=%~dp0"
if "!SCRIPT_DIR:~-1!"=="\" set "SCRIPT_DIR=!SCRIPT_DIR:~0,-1!"
set "RELAY_DIR=!SCRIPT_DIR!\sdr-relay"
set "RELAY_BIN=!RELAY_DIR!\build\Debug\sdr_relay.exe"
set "CLIENT_DIR=!SCRIPT_DIR!\client"

set "CMD=%~1"
if "!CMD!"=="" set "CMD=start"

echo.
echo   ==================================================
echo     AetherGuard - Software Defined Radio
echo     OS: Windows
echo   ==================================================
echo.

if /i "!CMD!"=="start"    goto :cmd_start
if /i "!CMD!"=="up"       goto :cmd_start
if /i "!CMD!"=="mock"     goto :cmd_mock
if /i "!CMD!"=="stop"     goto :cmd_stop
if /i "!CMD!"=="down"     goto :cmd_stop
if /i "!CMD!"=="relay"    goto :cmd_relay
if /i "!CMD!"=="docker"   goto :cmd_docker
if /i "!CMD!"=="frontend" goto :cmd_frontend
if /i "!CMD!"=="build"    goto :cmd_build
if /i "!CMD!"=="status"   goto :cmd_status

echo   Usage: start.bat {start^|mock^|stop^|relay^|docker^|frontend^|build^|status}
goto :eof


:: =============================================================================
:check_prereqs
:: =============================================================================
    echo   [..] Checking prerequisites...
    where cmake >nul 2>&1
    if !errorlevel! neq 0 echo   [!!] cmake not found. && exit /b 1
    where docker >nul 2>&1
    if !errorlevel! neq 0 echo   [!!] docker not found. && exit /b 1
    where node >nul 2>&1
    if !errorlevel! neq 0 echo   [!!] node not found. && exit /b 1
    echo   [OK] Prerequisites satisfied.
    echo.
    exit /b 0


:: =============================================================================
:build_relay
:: =============================================================================
    echo   [..] Preparing sdr-relay build...

    if not exist "!RELAY_DIR!\build" mkdir "!RELAY_DIR!\build"

    :: Configure if needed
    if exist "!RELAY_DIR!\build\CMakeCache.txt" goto :build_relay_compile
    echo   [..] Running cmake configure (first time setup)...
    pushd "!RELAY_DIR!\build"
    cmake .. -G "Visual Studio 17 2022" -A x64
    if !errorlevel! neq 0 popd && echo   [!!] CMake configure failed. && exit /b 1
    popd
    echo   [OK] CMake configured.

:build_relay_compile
    echo   [..] Building sdr-relay...
    cmake --build "!RELAY_DIR!\build" --config Debug
    if !errorlevel! neq 0 echo   [!!] Build failed. && exit /b 1
    if not exist "!RELAY_BIN!" echo   [!!] Binary not found. && exit /b 1
    echo   [OK] Relay built: !RELAY_BIN!
    echo.
    exit /b 0


:: =============================================================================
:install_frontend
:: =============================================================================
    if exist "!CLIENT_DIR!\node_modules" echo   [OK] Frontend dependencies present. && echo. && exit /b 0
    echo   [..] Installing frontend dependencies (first time setup)...
    pushd "!CLIENT_DIR!"
    call npm install
    if !errorlevel! neq 0 popd && echo   [!!] npm install failed. && exit /b 1
    popd
    echo   [OK] Frontend dependencies installed.
    echo.
    exit /b 0


:: =============================================================================
:start_relay_process
:: =============================================================================
    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if !errorlevel! equ 0 echo   [OK] Relay is already running. && exit /b 0

    echo   [..] Starting sdr-relay...
    start "AetherGuard - SDR Relay" /min "!RELAY_BIN!"
    timeout /t 3 /nobreak >nul

    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if !errorlevel! neq 0 echo   [!!] Relay failed to start. && exit /b 1

    echo   [OK] Relay running (Data: 7373, Ctrl: 7374)
    echo.
    exit /b 0


:: =============================================================================
:start_docker_process
:: =============================================================================
    echo   [..] Starting Docker pipeline (SDR_MODE=!SDR_MODE!)...
    set "SDR_RELAY_HOST=host.docker.internal"
    start "AetherGuard - Docker" cmd /k "cd /d !SCRIPT_DIR! && docker compose up --build"
    timeout /t 5 /nobreak >nul
    echo   [OK] Docker pipeline launching in separate window.
    echo.
    exit /b 0


:: =============================================================================
:start_frontend_process
:: =============================================================================
    echo   [..] Starting frontend dev server...
    start "AetherGuard - Frontend" cmd /k "cd /d !CLIENT_DIR! && npm run dev"
    timeout /t 3 /nobreak >nul
    echo   [OK] Frontend launching in separate window.
    echo.
    exit /b 0


:: =============================================================================
:: COMMANDS
:: =============================================================================

:: ---- START: try relay, fall back to mock ----
:cmd_start
    call :check_prereqs
    if !errorlevel! neq 0 goto :eof
    call :install_frontend
    if !errorlevel! neq 0 goto :eof

    set "SDR_MODE=live"

    call :build_relay
    if !errorlevel! neq 0 goto :start_use_mock

    call :start_relay_process
    if !errorlevel! neq 0 goto :start_use_mock

    goto :start_launch

:start_use_mock
    echo.
    echo   [!!] Relay unavailable — switching to MOCK MODE.
    echo       No SDR hardware needed. All features work with simulated data.
    echo.
    set "SDR_MODE=mock"

:start_launch
    call :start_docker_process
    call :start_frontend_process

    echo   ==================================================
    echo     AetherGuard is running!
    if "!SDR_MODE!"=="mock" echo     MODE: MOCK - no SDR hardware
    if "!SDR_MODE!"=="live" echo     MODE: LIVE - SDR relay active
    echo.
    echo     Frontend:  Check the frontend window for URL
    echo     API:       http://localhost:8080
    if "!SDR_MODE!"=="live" echo     SDR Relay: localhost:7373 / 7374
    echo.
    echo     To stop:   start.bat stop
    echo   ==================================================
    echo.
    goto :eof


:: ---- MOCK: skip relay entirely ----
:cmd_mock
    call :check_prereqs
    if !errorlevel! neq 0 goto :eof
    call :install_frontend
    if !errorlevel! neq 0 goto :eof

    set "SDR_MODE=mock"
    echo   [OK] Starting in MOCK MODE - no SDR hardware needed
    echo.

    call :start_docker_process
    call :start_frontend_process

    echo   ==================================================
    echo     AetherGuard is running in MOCK MODE!
    echo.
    echo     Frontend:  Check the frontend window for URL
    echo     API:       http://localhost:8080
    echo     SDR:       Simulated - 440 Hz tone, fake stations
    echo.
    echo     To stop:   start.bat stop
    echo   ==================================================
    echo.
    goto :eof


:: ---- STOP ----
:cmd_stop
    echo   [..] Stopping all AetherGuard processes...
    echo.
    taskkill /FI "WINDOWTITLE eq AetherGuard - Frontend" /T /F >nul 2>&1
    echo   [..] Stopping Docker containers...
    cd /d "!SCRIPT_DIR!"
    docker compose down >nul 2>&1
    taskkill /FI "WINDOWTITLE eq AetherGuard - Docker" /T /F >nul 2>&1
    echo   [OK] Docker containers stopped.
    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if !errorlevel! equ 0 taskkill /IM sdr_relay.exe /F >nul 2>&1 && echo   [OK] Relay stopped.
    echo.
    echo   [OK] Everything stopped.
    goto :eof


:: ---- RELAY only ----
:cmd_relay
    call :check_prereqs
    if !errorlevel! neq 0 goto :eof
    call :build_relay
    if !errorlevel! neq 0 goto :eof
    call :start_relay_process
    echo.
    echo   Relay is running. Start Docker and frontend separately:
    echo     start.bat docker
    echo     start.bat frontend
    goto :eof


:: ---- DOCKER only ----
:cmd_docker
    if not defined SDR_MODE set "SDR_MODE=live"
    set "SDR_RELAY_HOST=host.docker.internal"
    echo   [..] Starting Docker pipeline (SDR_MODE=!SDR_MODE!)...
    cd /d "!SCRIPT_DIR!"
    docker compose up --build
    goto :eof


:: ---- FRONTEND only ----
:cmd_frontend
    call :install_frontend
    if !errorlevel! neq 0 goto :eof
    echo   [..] Starting frontend dev server...
    cd /d "!CLIENT_DIR!"
    npm run dev
    goto :eof


:: ---- BUILD only ----
:cmd_build
    call :check_prereqs
    if !errorlevel! neq 0 goto :eof
    call :build_relay
    goto :eof


:: ---- STATUS ----
:cmd_status
    echo.
    echo   === AetherGuard Status ===
    echo.
    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if !errorlevel! equ 0 (echo     Relay:          RUNNING) else (echo     Relay:          STOPPED)
    sc query SDRplayAPIService >nul 2>&1
    if !errorlevel! equ 0 (echo     SDRplay API:    INSTALLED) else (echo     SDRplay API:    NOT FOUND)
    echo.
    for %%C in (ag-redis ag-api ag-ai ag-sdr-live) do (
        docker ps --format "{{.Names}}" 2>nul | find "%%C" >nul 2>&1
        if !errorlevel! equ 0 (echo     %%C:        RUNNING) else (echo     %%C:        STOPPED)
    )
    echo.
    goto :eof