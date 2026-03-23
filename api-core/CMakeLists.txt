cmake_minimum_required(VERSION 3.14) 
project(AetherGuardAPI)

set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(NOMINMAX)

# 1. Find Pre-Installed Dependencies (Instant via Docker)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# 2. Include Directories 
# (Crow and JSON are already natively in /usr/local/include via the base image!)
include_directories(src)
include_directories(src/db)
include_directories(src/dbfunction/corefunction)
include_directories(src/dbfunction/filterfunction)
include_directories(src/openai)


# 3. Find API Source Files
# NOTE: Make sure you do NOT copy the sdr, iqwav2wav, or whispertinytest folders into the api-core/src!
file(GLOB_RECURSE API_SOURCES "src/*.cpp")

# 4. Define Executable
add_executable(api_core 
    ${API_SOURCES}
    src/db/sqlite3.c 
)

# 5. Link Libraries
target_link_libraries(api_core 
    Threads::Threads 
    ${CURL_LIBRARIES}
    hiredis
    sqlite3
)