@echo off
setlocal EnableDelayedExpansion
:: =============================================================================
:: start.bat — One-line launcher for AetherGuard (Windows)
:: =============================================================================
::
:: Usage:
::   start.bat              Start everything (relay + Docker + frontend)
::   start.bat stop         Stop everything
::   start.bat relay        Start only the native relay
::   start.bat docker       Start only the Docker pipeline
::   start.bat frontend     Start only the frontend dev server
::   start.bat build        Build/rebuild the relay without starting
::   start.bat status       Show what's running
::
:: On a fresh install this script will:
::   1. Create sdr-relay/build and run cmake configure
::   2. Build the relay binary
::   3. Install npm packages in client/ if needed
::   4. Start all three processes
::
:: =============================================================================

set "SCRIPT_DIR=%~dp0"
:: Remove trailing backslash for cleaner paths
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "RELAY_DIR=%SCRIPT_DIR%\sdr-relay"
set "RELAY_BIN=%RELAY_DIR%\build\Debug\sdr_relay.exe"
set "CLIENT_DIR=%SCRIPT_DIR%\client"

:: Default command
set "CMD=%~1"
if "%CMD%"=="" set "CMD=start"

:: ---- Banner ----
echo.
echo   ==================================================
echo     AetherGuard - Software Defined Radio
echo     OS: Windows
echo   ==================================================
echo.

:: ---- Route to command ----
if /i "%CMD%"=="start"    goto :cmd_start
if /i "%CMD%"=="up"       goto :cmd_start
if /i "%CMD%"=="stop"     goto :cmd_stop
if /i "%CMD%"=="down"     goto :cmd_stop
if /i "%CMD%"=="relay"    goto :cmd_relay
if /i "%CMD%"=="docker"   goto :cmd_docker
if /i "%CMD%"=="frontend" goto :cmd_frontend
if /i "%CMD%"=="build"    goto :cmd_build
if /i "%CMD%"=="status"   goto :cmd_status

echo   Usage: start.bat {start^|stop^|relay^|docker^|frontend^|build^|status}
echo.
echo     start     - Build relay, start relay + Docker + frontend
echo     stop      - Stop everything
echo     relay     - Build and start only the native relay
echo     docker    - Start only the Docker pipeline
echo     frontend  - Start only the frontend dev server
echo     build     - Build/rebuild the relay binary
echo     status    - Show what's running
goto :eof

:: =============================================================================
:: CHECK PREREQUISITES
:: =============================================================================
:check_prereqs
    echo   [..] Checking prerequisites...

    where cmake >nul 2>&1
    if %errorlevel% neq 0 (
        echo   [!!] cmake not found. Install CMake and add it to PATH.
        exit /b 1
    )

    where docker >nul 2>&1
    if %errorlevel% neq 0 (
        echo   [!!] docker not found. Install Docker Desktop for Windows.
        exit /b 1
    )

    where node >nul 2>&1
    if %errorlevel% neq 0 (
        echo   [!!] node not found. Install Node.js from https://nodejs.org
        exit /b 1
    )

    :: Check SDRplay API
    if not exist "C:\Program Files\SDRplay\API\x64\sdrplay_api.lib" (
        echo   [!!] SDRplay API not found at C:\Program Files\SDRplay\API
        echo       Download from: https://www.sdrplay.com/downloads/
        echo       Continuing anyway - cmake will report if it can't find it...
    )

    echo   [OK] Prerequisites satisfied.
    echo.
    exit /b 0

:: =============================================================================
:: BUILD RELAY (fresh-install safe)
:: =============================================================================
:build_relay
    echo   [..] Preparing sdr-relay build...

    :: Create build directory if it doesn't exist
    if not exist "%RELAY_DIR%\build" (
        echo   [..] Creating build directory...
        mkdir "%RELAY_DIR%\build"
    )

    :: Run cmake configure if CMakeCache doesn't exist (fresh install)
    if not exist "%RELAY_DIR%\build\CMakeCache.txt" (
        echo   [..] Running cmake configure ^(first time setup^)...
        pushd "%RELAY_DIR%\build"
        cmake .. -G "Visual Studio 17 2022" -A x64
        if %errorlevel% neq 0 (
            echo   [!!] CMake configure failed.
            echo       If Visual Studio 2022 is not installed, adjust the generator:
            echo         cmake .. -G "Visual Studio 16 2019" -A x64
            popd
            exit /b 1
        )
        popd
        echo   [OK] CMake configured.
    )

    :: Build
    echo   [..] Building sdr-relay...
    cmake --build "%RELAY_DIR%\build" --config Debug
    if %errorlevel% neq 0 (
        echo   [!!] Build failed. Check errors above.
        exit /b 1
    )

    if not exist "%RELAY_BIN%" (
        echo   [!!] Build completed but binary not found at:
        echo       %RELAY_BIN%
        exit /b 1
    )

    echo   [OK] Relay built: %RELAY_BIN%
    echo.
    exit /b 0

:: =============================================================================
:: INSTALL FRONTEND DEPS (fresh-install safe)
:: =============================================================================
:install_frontend
    if not exist "%CLIENT_DIR%\node_modules" (
        echo   [..] Installing frontend dependencies ^(first time setup^)...
        pushd "%CLIENT_DIR%"
        call npm install
        if %errorlevel% neq 0 (
            echo   [!!] npm install failed.
            popd
            exit /b 1
        )
        popd
        echo   [OK] Frontend dependencies installed.
    ) else (
        echo   [OK] Frontend dependencies already installed.
    )
    echo.
    exit /b 0

:: =============================================================================
:: START RELAY
:: =============================================================================
:start_relay_process
    :: Check if already running
    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if %errorlevel% equ 0 (
        echo   [OK] Relay is already running.
        exit /b 0
    )

    echo   [..] Starting sdr-relay...
    start "AetherGuard - SDR Relay" /min "%RELAY_BIN%"

    :: Wait for it to initialize
    timeout /t 3 /nobreak >nul

    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if %errorlevel% neq 0 (
        echo   [!!] Relay failed to start. Check that:
        echo       1. SDRplay hardware is connected via USB
        echo       2. SDRplay API service is running ^(check Services^)
        echo       3. No other program is using the SDR
        exit /b 1
    )

    echo   [OK] Relay running ^(Data: 7373, Ctrl: 7374^)
    echo.
    exit /b 0

:: =============================================================================
:: START DOCKER
:: =============================================================================
:start_docker_process
    echo   [..] Starting Docker pipeline...
    set "SDR_RELAY_HOST=host.docker.internal"
    start "AetherGuard - Docker" /min cmd /c "cd /d "%SCRIPT_DIR%" && docker compose up --build"

    :: Give containers a moment to pull/build
    timeout /t 5 /nobreak >nul
    echo   [OK] Docker pipeline starting ^(building containers...^)
    echo.
    exit /b 0

:: =============================================================================
:: START FRONTEND
:: =============================================================================
:start_frontend_process
    echo   [..] Starting frontend dev server...
    start "AetherGuard - Frontend" /min cmd /c "cd /d "%CLIENT_DIR%" && npm run dev"

    timeout /t 3 /nobreak >nul
    echo   [OK] Frontend dev server starting...
    echo.
    exit /b 0

:: =============================================================================
:: COMMANDS
:: =============================================================================

:: ---- START: everything ----
:cmd_start
    call :check_prereqs
    if %errorlevel% neq 0 goto :eof

    call :build_relay
    if %errorlevel% neq 0 goto :eof

    call :install_frontend
    if %errorlevel% neq 0 goto :eof

    call :start_relay_process
    if %errorlevel% neq 0 goto :eof

    call :start_docker_process

    call :start_frontend_process

    echo.
    echo   ==================================================
    echo     AetherGuard is running!
    echo.
    echo     Frontend:  Check terminal for dev server URL
    echo     API:       http://localhost:8080
    echo     SDR Relay: localhost:7373 ^(data^) / 7374 ^(ctrl^)
    echo.
    echo     To stop:   start.bat stop
    echo   ==================================================
    echo.
    goto :eof

:: ---- STOP: everything ----
:cmd_stop
    echo   [..] Stopping all AetherGuard processes...
    echo.

    :: Stop frontend (node processes from npm run dev)
    tasklist /FI "WINDOWTITLE eq AetherGuard - Frontend" 2>nul | find /I "cmd.exe" >nul
    if %errorlevel% equ 0 (
        taskkill /FI "WINDOWTITLE eq AetherGuard - Frontend" /T /F >nul 2>&1
        echo   [OK] Frontend stopped.
    )

    :: Stop Docker
    echo   [..] Stopping Docker containers...
    cd /d "%SCRIPT_DIR%"
    docker compose down >nul 2>&1
    :: Also kill the docker compose window
    taskkill /FI "WINDOWTITLE eq AetherGuard - Docker" /T /F >nul 2>&1
    echo   [OK] Docker containers stopped.

    :: Stop relay
    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if %errorlevel% equ 0 (
        taskkill /IM sdr_relay.exe /F >nul 2>&1
        echo   [OK] Relay stopped.
    ) else (
        echo   [--] Relay was not running.
    )

    echo.
    echo   [OK] Everything stopped.
    goto :eof

:: ---- RELAY only ----
:cmd_relay
    call :check_prereqs
    if %errorlevel% neq 0 goto :eof
    call :build_relay
    if %errorlevel% neq 0 goto :eof
    call :start_relay_process
    echo.
    echo   Relay is running. Start Docker and frontend separately:
    echo     start.bat docker
    echo     start.bat frontend
    goto :eof

:: ---- DOCKER only ----
:cmd_docker
    set "SDR_RELAY_HOST=host.docker.internal"
    echo   [..] Starting Docker pipeline...
    cd /d "%SCRIPT_DIR%"
    docker compose up --build
    goto :eof

:: ---- FRONTEND only ----
:cmd_frontend
    call :install_frontend
    if %errorlevel% neq 0 goto :eof
    echo   [..] Starting frontend dev server...
    cd /d "%CLIENT_DIR%"
    npm run dev
    goto :eof

:: ---- BUILD only ----
:cmd_build
    call :check_prereqs
    if %errorlevel% neq 0 goto :eof
    call :build_relay
    goto :eof

:: ---- STATUS ----
:cmd_status
    echo.
    echo   === AetherGuard Status ===
    echo.

    :: Relay
    tasklist /FI "IMAGENAME eq sdr_relay.exe" 2>nul | find /I "sdr_relay.exe" >nul
    if %errorlevel% equ 0 (
        echo     Relay:          RUNNING
    ) else (
        echo     Relay:          STOPPED
    )

    :: SDRplay service
    sc query SDRplayAPIService >nul 2>&1
    if %errorlevel% equ 0 (
        sc query SDRplayAPIService | find "RUNNING" >nul 2>&1
        if %errorlevel% equ 0 (
            echo     SDRplay API:    RUNNING
        ) else (
            echo     SDRplay API:    STOPPED
        )
    ) else (
        echo     SDRplay API:    NOT INSTALLED
    )

    :: Docker containers
    echo.
    for %%C in (ag-redis ag-api ag-ai ag-sdr-live) do (
        docker ps --format "{{.Names}}" 2>nul | find "%%C" >nul 2>&1
        if !errorlevel! equ 0 (
            echo     %%C:        RUNNING
        ) else (
            echo     %%C:        STOPPED
        )
    )
    echo.
    goto :eof
