cmake_minimum_required(VERSION 3.14) 
project(SDRDaemon)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)

# Include Directories 
include_directories(src)
include_directories(src/sdr)
include_directories(src/iqtowav)
include_directories(/usr/local/include)

# =============================================================================
# MODIFIED: SDRplay API library removed.  USB hardware access now lives in
# the native sdr-relay host application.  This container is pure DSP + TCP.
# =============================================================================

# Find Source Files — exclude sdr_handler.cpp (moved to sdr-relay)
file(GLOB_RECURSE SDR_SOURCES "src/*.cpp")
list(FILTER SDR_SOURCES EXCLUDE REGEX ".*sdr_handler\\.cpp$")

# Define Executable
add_executable(sdr_daemon ${SDR_SOURCES})

# Link Libraries (no sdrplay_api)
target_link_libraries(sdr_daemon 
    Threads::Threads 
    hiredis
    liquid 
    sndfile
)