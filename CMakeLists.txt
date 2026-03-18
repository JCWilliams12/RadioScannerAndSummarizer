cmake_minimum_required(VERSION 3.14) 
project(AetherGuardServer)

set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(NOMINMAX)

# --- 0. DEPENDENCIES (Whisper.cpp) ---
include(FetchContent)
message(STATUS "Fetching whisper.cpp...")

FetchContent_Declare(
    whisper_cpp
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        master
)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Disable whisper tests" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Disable whisper examples" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "Disable whisper server" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libraries" FORCE)
FetchContent_MakeAvailable(whisper_cpp)

# --- 0.25 DEPENDENCIES (Llama.cpp) ---
message(STATUS "Fetching llama.cpp...")
FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
    GIT_TAG        master
)

set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Disable llama tests" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Disable llama examples" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Disable llama server" FORCE)
FetchContent_MakeAvailable(llama_cpp)

# --- 0.5. DEPENDENCIES (libcurl from source) ---
message(STATUS "Fetching libcurl...")
FetchContent_Declare(
    curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG        curl-8_6_0 
)

set(BUILD_CURL_EXE OFF CACHE BOOL "Disable curl executable" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static curl" FORCE)
set(HTTP_ONLY ON CACHE BOOL "Only enable HTTP/HTTPS protocols" FORCE)

if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native crypto" FORCE)
else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "Use Linux native crypto" FORCE)
endif()

FetchContent_MakeAvailable(curl)

# --- 1. MODEL DOWNLOADER (Whisper) ---
set(WHISPER_MODEL_FILENAME "ggml-base.en.bin")
set(WHISPER_MODEL_URL "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${WHISPER_MODEL_FILENAME}")
set(WHISPER_MODEL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/server/src/whispertinytest/${WHISPER_MODEL_FILENAME}")

if(NOT EXISTS "${WHISPER_MODEL_PATH}")
    message(STATUS "Whisper Model not found. Downloading to ${WHISPER_MODEL_PATH}...")
    file(DOWNLOAD ${WHISPER_MODEL_URL} ${WHISPER_MODEL_PATH} SHOW_PROGRESS STATUS DOWNLOAD_STATUS)
endif()

# --- 1.5 MODEL DOWNLOADER (Phi-3 Mini for Llama.cpp) ---
set(PHI3_MODEL_FILENAME "Phi-3-mini-4k-instruct-q4.gguf")
set(PHI3_MODEL_URL "https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-gguf/resolve/main/${PHI3_MODEL_FILENAME}")
set(PHI3_MODEL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/server/src/ollamatest/${PHI3_MODEL_FILENAME}")

if(NOT EXISTS "${PHI3_MODEL_PATH}")
    message(STATUS "Phi-3 Model not found. Downloading to ${PHI3_MODEL_PATH}...")
    file(DOWNLOAD ${PHI3_MODEL_URL} ${PHI3_MODEL_PATH} SHOW_PROGRESS STATUS DOWNLOAD_STATUS)
endif()

# --- 2. COMPILER OPTIONS ---
if(MSVC)
    add_compile_options(/wd4267 /wd4244)
endif()

# --- 3. INCLUDE DIRECTORIES ---
include_directories(server/include)
include_directories(server/include/sdr)
include_directories(server/src/db)
include_directories(server/src/dbfunction/corefunction)
include_directories(server/src/dbfunction/filterfunction)
include_directories(server/src/iqwav2wav)
include_directories(server/src/ollamatest)
include_directories(server/src/openai)
include_directories(server/src/sdr) 
include_directories(server/src/whispertinytest)

if(WIN32)
    include_directories("C:/Program Files/SDRplay/API/inc")
else()
    include_directories("/usr/local/include")
endif()

find_package(Threads REQUIRED)

# --- 4. FIND SOURCES ---
file(GLOB_RECURSE APP_SOURCES "server/src/*.cpp")

# --- 5. DEFINE EXECUTABLE ---
add_executable(server 
    ${APP_SOURCES}
    server/src/db/sqlite3.c
)

# --- 6. LINKING LOGIC ---
# 1. Define common libraries all platforms need
set(COMMON_LIBS 
    Threads::Threads 
    whisper 
    llama 
    libcurl_static
)

# 2. Logic to find the SDRplay library specifically
if(WIN32)
    # Windows-specific search
    set(SDRPLAY_LIB "C:/Program Files/SDRplay/API/x64/sdrplay_api.lib")
    set(PLATFORM_LIBS Ws2_32 Crypt32 Wldap32 Normaliz)
else()
    # 1. Broad Search: Look for any version of the library in known locations
    find_library(SDRPLAY_LIB 
        NAMES 
            sdrplay_api              # Standard name
            sdrplay_api.so.3.15     # Specific Linux version
            sdrplay_api.so.3        # Generic Linux major version
            libsdrplay_api.dylib    # macOS name
        PATHS 
            /usr/local/lib          # Standard Linux location
            /usr/lib                # Linux fallback
            /Library/SDRplayAPI/3.15.1/lib  # Your specific Mac path
            /opt/sdrplay/lib        # Common alternative Linux path
    )

    # 2. Final Check
    if(NOT SDRPLAY_LIB)
        message(FATAL_ERROR "SDRplay API not found! Verified /usr/local/lib, but CMake still can't see it. Check permissions or run 'sudo ldconfig'.")
    else()
        message(STATUS "Found SDRplay API at: ${SDRPLAY_LIB}")
    endif()

    set(PLATFORM_LIBS dl)
endif()

# 3. Final link command
target_link_libraries(server 
    ${COMMON_LIBS}
    ${PLATFORM_LIBS}
    ${SDRPLAY_LIB}
)

if(WIN32)
    add_custom_command(TARGET server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "C:/Program Files/SDRplay/API/x64/sdrplay_api.dll"
        $<TARGET_FILE_DIR:server>
    )
endif()

# --- 7. POST-BUILD (Environment and Models) ---
set(ENV_FILE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.env")

add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WHISPER_MODEL_PATH}"
    $<TARGET_FILE_DIR:server>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${PHI3_MODEL_PATH}"
    $<TARGET_FILE_DIR:server>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ENV_FILE_PATH}"
    $<TARGET_FILE_DIR:server>
)