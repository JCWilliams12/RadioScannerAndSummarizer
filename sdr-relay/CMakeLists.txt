cmake_minimum_required(VERSION 3.14)
project(SdrRelay)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)

# =============================================================================
# SDRplay API — must be installed natively on the host.
#   Linux:   Run SDRplay_RSP_API-Linux-3.15.2.run
#   macOS:   Install SDRplay API from sdrplay.com
#   Windows: Install SDRplay API + use vcpkg or set paths manually
# =============================================================================
find_library(SDRPLAY_LIB
    NAMES sdrplay_api
    PATHS /usr/local/lib
          /opt/sdrplay/lib
          "C:/Program Files/SDRplay/API/x64"
)

if(NOT SDRPLAY_LIB)
    message(FATAL_ERROR
        "SDRplay API not found. Install from https://www.sdrplay.com/downloads/\n"
        "  Linux:   sudo ./SDRplay_RSP_API-Linux-3.15.2.run\n"
        "  macOS:   Install the macOS package from sdrplay.com\n"
        "  Windows: Install API + set CMAKE_PREFIX_PATH")
endif()

message(STATUS "Found SDRplay API at: ${SDRPLAY_LIB}")

# SDRplay headers
find_path(SDRPLAY_INCLUDE
    NAMES sdrplay_api.h
    PATHS /usr/local/include
          /opt/sdrplay/include
          "C:/Program Files/SDRplay/API/inc"
)

# =============================================================================
# nlohmann/json — lightweight header-only JSON for the control protocol
# =============================================================================
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# =============================================================================
# Build the relay
# =============================================================================
add_executable(sdr_relay
    main.cpp
    sdr_handler.cpp
)

target_include_directories(sdr_relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDRPLAY_INCLUDE}
)

target_link_libraries(sdr_relay
    Threads::Threads
    ${SDRPLAY_LIB}
    nlohmann_json::nlohmann_json
)

# On Linux, may need -lrt for clock functions
if(UNIX AND NOT APPLE)
    target_link_libraries(sdr_relay rt)
endif()

# On Windows, link Winsock
if(WIN32)
    target_link_libraries(sdr_relay ws2_32)
endif()