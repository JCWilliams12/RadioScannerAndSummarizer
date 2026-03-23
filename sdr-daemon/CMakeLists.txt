cmake_minimum_required(VERSION 3.14) 
project(SDRDaemon)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)

# Include Directories 
include_directories(src)
include_directories(src/sdr)
include_directories(src/iqtowav)
include_directories(/usr/local/include)

# Find the SDRplay library specifically
find_library(SDRPLAY_LIB 
    NAMES sdrplay_api
    PATHS /usr/local/lib
    NO_DEFAULT_PATH
)

if(NOT SDRPLAY_LIB)
    message(FATAL_ERROR "Linker cannot find libsdrplay_api.so in /usr/local/lib. Check Dockerfile symlink logic.")
endif()

message(STATUS "Found SDRplay API at: ${SDRPLAY_LIB}")

# Find Source Files
file(GLOB_RECURSE SDR_SOURCES "src/*.cpp")

# Define Executable
add_executable(sdr_daemon ${SDR_SOURCES})

# Link Libraries
target_link_libraries(sdr_daemon 
    Threads::Threads 
    hiredis
    liquid 
    sndfile
    ${SDRPLAY_LIB}
)