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

# --- 1. MODEL DOWNLOADER ---
set(MODEL_FILENAME "ggml-base.en.bin")
set(MODEL_URL "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${MODEL_FILENAME}")
set(MODEL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/server/src/whispertinytest/${MODEL_FILENAME}")

if(NOT EXISTS "${MODEL_PATH}")
    message(STATUS "Model not found. Downloading to ${MODEL_PATH}...")
    file(DOWNLOAD ${MODEL_URL} ${MODEL_PATH} SHOW_PROGRESS STATUS DOWNLOAD_STATUS)
endif()

# --- 2. COMPILER OPTIONS ---
if(MSVC)
    add_compile_options(/wd4267 /wd4244)
endif()

# --- 3. INCLUDE DIRECTORIES ---
# Ensure SDR and Crow headers are accessible
include_directories(server/include)
include_directories(server/include/sdr)
include_directories(server/src/db)
include_directories(server/src/dbfunction/corefunction)
include_directories(server/src/dbfunction/filterfunction)
include_directories(server/src/iqwav2wav)
include_directories(server/src/ollamatest)
include_directories(server/src/openai)
include_directories(server/src/sdr) # Location of sdr_handler and iqtowav
include_directories(server/src/whispertinytest)

# SDRplay API header location 
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
if(WIN32)
    target_link_libraries(server 
        Threads::Threads 
        Ws2_32 
        Crypt32    
        Wldap32    
        Normaliz   
        whisper 
        libcurl_static
        "C:/Program Files/SDRplay/API/x64/sdrplay_api.lib" # [cite: 1647, 1655]
    )
else()
    target_link_libraries(server 
        Threads::Threads 
        dl 
        whisper 
        libcurl_static
        "/Library/SDRplayAPI/3.15.1/lib/libsdrplay_api.so.3.15" 
        "/usr/local/lib/libsdrplay_api.dylib"
        "/usr/local/lib/libsdrplay_api.so.3"
        # Links against /usr/local/lib/libsdrplay_api.so 
    )
endif()

if(WIN32)
    # Copy the SDRplay DLL to the output directory [cite: 1655]
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
    "${MODEL_PATH}"
    $<TARGET_FILE_DIR:server>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ENV_FILE_PATH}"
    $<TARGET_FILE_DIR:server>
)