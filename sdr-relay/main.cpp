// =============================================================================
// sdr-relay/main.cpp — Native host SDR relay server
// =============================================================================
// This is the thin native application that runs directly on the host OS.
// It opens the SDR via USB (full interrupt rate, no WSL2 bottleneck) and
// exposes two TCP ports:
//
//   DATA port (7373): streams framed IQ samples to connected clients
//   CTRL port (7374): accepts JSON commands, returns JSON events
//
// The entire Docker pipeline (sdr-daemon, api-core, ai-worker, frontend)
// connects here for IQ data and hardware control.
// =============================================================================

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <thread>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <csignal>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>

// =============================================================================
// Cross-platform networking
// =============================================================================
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    using ssize_t   = int;
    using socklen_t = int;
    using socket_t  = SOCKET;

    #define MSG_NOSIGNAL 0

    inline int close_socket(socket_t fd) { return closesocket(fd); }

    inline int setsockopt_compat(socket_t s, int level, int optname,
                                 const void* optval, int optlen) {
        return setsockopt(s, level, optname,
                          reinterpret_cast<const char*>(optval), optlen);
    }

    inline ssize_t send_compat(socket_t s, const void* buf, size_t len, int flags) {
        return send(s, reinterpret_cast<const char*>(buf), static_cast<int>(len), flags);
    }

    inline ssize_t recv_compat(socket_t s, void* buf, size_t len, int flags) {
        return recv(s, reinterpret_cast<char*>(buf), static_cast<int>(len), flags);
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>

    using socket_t = int;

    inline int close_socket(socket_t fd) { return close(fd); }

    inline int setsockopt_compat(socket_t s, int level, int optname,
                                 const void* optval, socklen_t optlen) {
        return setsockopt(s, level, optname, optval, optlen);
    }

    inline ssize_t send_compat(socket_t s, const void* buf, size_t len, int flags) {
        return send(s, buf, len, flags);
    }

    inline ssize_t recv_compat(socket_t s, void* buf, size_t len, int flags) {
        return recv(s, buf, len, flags);
    }
#endif

// SDR hardware
#include <sdrplay_api.h>
#include "sdr_handler.hpp"
#include "relay_config.hpp"

// JSON (fetched by CMake — see CMakeLists.txt)
#include <nlohmann/json.hpp>
using json = nlohmann::json;


// =======================================================
// SHARED STATE (externs resolved by sdr_handler.cpp)
// =======================================================
enum class DeviceMode { IDLE, SCANNING, RECORDING, LIVE_LISTEN };
std::atomic<DeviceMode> g_mode(DeviceMode::IDLE);
std::atomic<bool>       g_stream_gap(false);

// Heartbeat (used by scanner's getValidatedPower)
std::mutex              g_heartbeat_mtx;
std::condition_variable g_heartbeat_cv;
std::atomic<long>       g_heartbeat_count(0);

// Active control client — allows async operations (like SCAN) to send
// responses back on the control socket from a worker thread.
static std::atomic<socket_t> g_ctrl_client_fd(static_cast<socket_t>(-1));
static std::mutex            g_ctrl_send_mtx;

static void sendCtrlResponse(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(g_ctrl_send_mtx);
    socket_t fd = g_ctrl_client_fd.load(std::memory_order_relaxed);
    if (fd == static_cast<socket_t>(-1)) return;
    std::string msg = json_str + "\n";
    send_compat(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
}


// =======================================================
// RELAY IQ RING BUFFER (USB callback -> TCP sender)
// =======================================================
struct RelaySlot {
    std::array<int16_t, relay::SLOT_SAMPLES * 2> data;
    uint32_t num_samples;
    std::atomic<bool> ready{false};
};

static RelaySlot           g_relay_pool[relay::POOL_SIZE];
static std::atomic<int>    g_relay_write(0);
static std::atomic<int>    g_relay_read(0);
static std::condition_variable g_relay_cv;
static std::mutex              g_relay_mtx;


// =======================================================
// broadcastAudio() — called by sdr_handler.cpp's USB callback
// =======================================================
void broadcastAudio(const std::vector<int16_t>& iqBuffer) {
    if (++g_heartbeat_count % 100 == 0) {
        g_heartbeat_cv.notify_all();
    }

    if (g_mode == DeviceMode::SCANNING || iqBuffer.empty()) return;

    uint32_t num_iq_pairs = static_cast<uint32_t>(iqBuffer.size() / 2);
    if (num_iq_pairs > relay::SLOT_SAMPLES) num_iq_pairs = relay::SLOT_SAMPLES;

    int idx = g_relay_write.load(std::memory_order_relaxed) % relay::POOL_SIZE;
    RelaySlot& slot = g_relay_pool[idx];

    if (slot.ready.load(std::memory_order_acquire)) {
        return;
    }

    std::memcpy(slot.data.data(), iqBuffer.data(), num_iq_pairs * 2 * sizeof(int16_t));
    slot.num_samples = num_iq_pairs;
    slot.ready.store(true, std::memory_order_release);
    g_relay_write.fetch_add(1, std::memory_order_relaxed);
    g_relay_cv.notify_one();
}


// =======================================================
// TCP DATA CLIENT TRACKING
// =======================================================
static std::mutex         g_clients_mtx;
static std::vector<socket_t> g_data_clients;

static void addDataClient(socket_t fd) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    g_data_clients.push_back(fd);
    std::cout << "[DATA] Client connected (fd=" << fd
              << ", total=" << g_data_clients.size() << ")" << std::endl;
}

static void removeDataClient(socket_t fd) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    g_data_clients.erase(
        std::remove(g_data_clients.begin(), g_data_clients.end(), fd),
        g_data_clients.end());
    close_socket(fd);
    std::cout << "[DATA] Client disconnected (fd=" << fd
              << ", remaining=" << g_data_clients.size() << ")" << std::endl;
}


// =======================================================
// TCP DATA SENDER THREAD
// =======================================================
void dataSenderThread() {
    const size_t max_frame = 8 + (relay::SLOT_SAMPLES * 2 * sizeof(int16_t));
    std::vector<uint8_t> frame(max_frame);

    while (true) {
        int idx = g_relay_read.load(std::memory_order_relaxed) % relay::POOL_SIZE;
        RelaySlot& slot = g_relay_pool[idx];

        if (!slot.ready.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(g_relay_mtx);
            g_relay_cv.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }

        uint32_t num_samples = slot.num_samples;
        size_t payload_bytes = num_samples * 2 * sizeof(int16_t);

        uint32_t net_magic  = htonl(0x49515043);
        uint32_t net_count  = htonl(num_samples);
        std::memcpy(frame.data(),     &net_magic, 4);
        std::memcpy(frame.data() + 4, &net_count, 4);
        std::memcpy(frame.data() + 8, slot.data.data(), payload_bytes);

        size_t frame_len = 8 + payload_bytes;

        slot.ready.store(false, std::memory_order_release);
        g_relay_read.fetch_add(1, std::memory_order_relaxed);

        std::vector<socket_t> dead_fds;
        {
            std::lock_guard<std::mutex> lock(g_clients_mtx);
            for (socket_t fd : g_data_clients) {
                ssize_t sent = send_compat(fd, frame.data(), frame_len, MSG_NOSIGNAL);
                if (sent != (ssize_t)frame_len) {
                    dead_fds.push_back(fd);
                }
            }
        }
        for (socket_t fd : dead_fds) {
            removeDataClient(fd);
        }
    }
}


// =======================================================
// TCP DATA ACCEPT THREAD
// =======================================================
void dataAcceptThread() {
    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt_compat(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(relay::DATA_PORT);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[DATA] Bind failed on port " << relay::DATA_PORT
                  << ": " << strerror(errno) << std::endl;
        return;
    }
    listen(srv, relay::LISTEN_BACKLOG);
    std::cout << "[DATA] Listening on port " << relay::DATA_PORT << std::endl;

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        socket_t client_fd = accept(srv, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        int flag = 1;
        setsockopt_compat(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        int sndbuf = relay::TCP_SNDBUF_SIZE;
        setsockopt_compat(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        addDataClient(client_fd);
    }
}


// =======================================================
// HARDWARE HELPERS
// =======================================================
extern std::atomic<float> g_fast_power;

void setBandwidth(SdrHandler* sdr, sdrplay_api_Bw_MHzT bw, sdrplay_api_If_kHzT ifMode) {
    sdrplay_api_DeviceParamsT* params = nullptr;
    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
    if (!params || !params->rxChannelA) return;

    params->rxChannelA->tunerParams.bwType = bw;
    params->rxChannelA->tunerParams.ifType = ifMode;
    sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
        (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Tuner_BwType | sdrplay_api_Update_Tuner_IfType),
        sdrplay_api_Update_Ext1_None);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    params->rxChannelA->ctrlParams.decimation.enable          = 1;
    params->rxChannelA->ctrlParams.decimation.decimationFactor = relay::DECIMATION_FACTOR;
    params->rxChannelA->ctrlParams.decimation.wideBandSignal   = 0;
    sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
        sdrplay_api_Update_Ctrl_Decimation, sdrplay_api_Update_Ext1_None);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "[HW] setBandwidth complete." << std::endl;
}

float getValidatedPower(SdrHandler* sdr) {
    sdr->ClearPowerHistory();
    long start_count = g_heartbeat_count.load();
    std::unique_lock<std::mutex> lock(g_heartbeat_mtx);
    bool success = g_heartbeat_cv.wait_for(lock, std::chrono::milliseconds(500), [&]{
        return (g_heartbeat_count.load() - start_count) >= 4;
    });
    if (!success) {
        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                           sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
        return -100.0f;
    }
    return sdr->GetCurrentPower();
}


// =======================================================
// CONTROL COMMAND DISPATCH
// =======================================================
std::string handleCommand(SdrHandler* sdr, const json& cmd) {
    std::string command = cmd.value("command", "");

    // ---- TUNE ----
    if (command == "TUNE") {
        double freq = cmd.value("freq", 88.1);
        sdr->TuneFrequency(freq * 1000000.0);
        std::cout << "[CTRL] Tuned to " << freq << " MHz" << std::endl;
        return "";
    }

    // ---- CONFIGURE: set BW, IF mode, AGC, LNA, tune ----
    // FIX: This MUST be synchronous.  The SDRplay API is not thread-safe —
    // if we spawn a thread here, it can race with the USB callback thread
    // or a concurrent SCAN thread calling sdrplay_api_Update().  The 200ms
    // block is acceptable; only SCAN (10+ seconds) needs to be async.
    if (command == "CONFIGURE") {
        double freq     = cmd.value("freq", 88.1);
        std::string bw  = cmd.value("bw", "BW_0_200");
        std::string agc = cmd.value("agc", "off");
        int lna         = cmd.value("lna", 4);

        sdrplay_api_Bw_MHzT bwEnum = sdrplay_api_BW_0_200;
        if (bw == "BW_1_536")       bwEnum = sdrplay_api_BW_1_536;
        else if (bw == "BW_0_600")  bwEnum = sdrplay_api_BW_0_600;
        else if (bw == "BW_0_300")  bwEnum = sdrplay_api_BW_0_300;

        setBandwidth(sdr, bwEnum, sdrplay_api_IF_Zero);

        sdrplay_api_DeviceParamsT* params = nullptr;
        sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
        if (params && params->rxChannelA) {
            params->rxChannelA->tunerParams.gain.LNAstate = lna;
            params->rxChannelA->ctrlParams.agc.enable =
                (agc == "off") ? sdrplay_api_AGC_DISABLE : sdrplay_api_AGC_50HZ;
            sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                sdrplay_api_Update_Ext1_None);
        }

        sdr->TuneFrequency(freq * 1000000.0);
        std::cout << "[CTRL] Configured: freq=" << freq << " bw=" << bw
                  << " agc=" << agc << " lna=" << lna << std::endl;

        return json({{"event", "configure_ack"}}).dump();
    }

    // ---- SCAN: wideband FM scan — runs asynchronously ----
    if (command == "SCAN") {
        std::cout << "[CTRL] Starting wideband scan (async)..." << std::endl;
        g_mode = DeviceMode::SCANNING;

        std::thread([sdr]() {
            setBandwidth(sdr, sdrplay_api_BW_0_200, sdrplay_api_IF_Zero);

            sdrplay_api_DeviceParamsT* params = nullptr;
            sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
            if (params && params->rxChannelA) {
                params->rxChannelA->ctrlParams.agc.enable     = sdrplay_api_AGC_DISABLE;
                params->rxChannelA->tunerParams.gain.LNAstate = 8;
                sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                    (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                    sdrplay_api_Update_Ext1_None);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            sdr->TuneFrequency(87.9 * 1000000.0);
            float noiseFloor = getValidatedPower(sdr);
            if (noiseFloor <= -99.0f) noiseFloor = -85.0f;
            float squelchThreshold = noiseFloor + relay::SCAN_SQUELCH_DB;

            std::cout << "[Scanner] Noise floor: " << noiseFloor
                      << " dBFS, squelch: " << squelchThreshold << " dBFS" << std::endl;

            json found_stations = json::array();
            double bestFreq     = 0.0;
            float  bestRssi     = -999.0f;
            bool   inCluster    = false;
            int    clusterSteps = 0;

            for (double freqMHz = relay::SCAN_START_MHZ;
                 freqMHz <= relay::SCAN_END_MHZ;
                 freqMHz += relay::SCAN_STEP_MHZ) {

                sdr->TuneFrequency(freqMHz * 1000000.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                float currentRssi = getValidatedPower(sdr);

                if (currentRssi >= squelchThreshold) {
                    inCluster = true;
                    clusterSteps++;
                    int freq10 = static_cast<int>(std::round(freqMHz * 10.0));
                    if (freq10 % 2 != 0) {
                        if (currentRssi > bestRssi) {
                            bestRssi = currentRssi;
                            bestFreq = freqMHz;
                        }
                    }
                } else {
                    if (inCluster) {
                        if (clusterSteps >= 2 && bestFreq > 0.0) {
                            std::string name = "FM " + std::to_string(bestFreq).substr(0, 5);
                            found_stations.push_back({{"freq", bestFreq}, {"name", name}});
                        }
                        inCluster    = false;
                        bestRssi     = -999.0f;
                        bestFreq     = 0.0;
                        clusterSteps = 0;
                    }
                }
            }

            if (inCluster && clusterSteps >= 2 && bestFreq > 0.0) {
                std::string name = "FM " + std::to_string(bestFreq).substr(0, 5);
                found_stations.push_back({{"freq", bestFreq}, {"name", name}});
            }

            std::cout << "[Scanner] Complete. Found " << found_stations.size()
                      << " stations." << std::endl;

            g_mode = DeviceMode::IDLE;
            sendCtrlResponse(json({{"event", "scan_complete"}, {"stations", found_stations}}).dump());
        }).detach();

        return "";
    }

    std::cerr << "[CTRL] Unknown command: " << command << std::endl;
    return json({{"event", "error"}, {"msg", "unknown command: " + command}}).dump();
}


// =======================================================
// TCP CONTROL SERVER THREAD
// =======================================================
void controlServerThread(SdrHandler* sdr) {
    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt_compat(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(relay::CTRL_PORT);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[CTRL] Bind failed on port " << relay::CTRL_PORT
                  << ": " << strerror(errno) << std::endl;
        return;
    }
    listen(srv, relay::LISTEN_BACKLOG);
    std::cout << "[CTRL] Listening on port " << relay::CTRL_PORT << std::endl;

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        socket_t client_fd = accept(srv, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        g_ctrl_client_fd.store(client_fd, std::memory_order_release);
        std::cout << "[CTRL] Client connected." << std::endl;

        std::string line_buf;
        char chunk[4096];

        while (true) {
            ssize_t n = recv_compat(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;

            line_buf.append(chunk, n);

            size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                std::string line = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);
                if (line.empty()) continue;

                try {
                    json cmd = json::parse(line);
                    std::string response = handleCommand(sdr, cmd);

                    if (!response.empty()) {
                        sendCtrlResponse(response);
                    }
                } catch (const json::exception& e) {
                    std::cerr << "[CTRL] JSON parse error: " << e.what() << std::endl;
                    sendCtrlResponse(json({{"event","error"},{"msg","bad json"}}).dump());
                }
            }
        }

        g_ctrl_client_fd.store(static_cast<socket_t>(-1), std::memory_order_release);
        close_socket(client_fd);
        std::cout << "[CTRL] Client disconnected." << std::endl;
    }
}


// =======================================================
// MAIN
// =======================================================
int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[RELAY] WSAStartup failed." << std::endl;
        return 1;
    }
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::cout << "\n==========================================" << std::endl;
    std::cout << "  AetherGuard SDR Relay — Native Host App" << std::endl;
    std::cout << "  Data port : " << relay::DATA_PORT << std::endl;
    std::cout << "  Ctrl port : " << relay::CTRL_PORT << std::endl;
    std::cout << "==========================================" << std::endl;

    SdrHandler sdr;
    if (!sdr.InitializeAPI()) {
        std::cerr << "[RELAY] SDR initialization failed." << std::endl;
        return 1;
    }
    if (!sdr.StartStream(relay::DEFAULT_FREQ_HZ)) {
        std::cerr << "[RELAY] SDR stream start failed." << std::endl;
        return 1;
    }

    std::cout << "[RELAY] SDR streaming. Launching TCP servers..." << std::endl;

    std::thread(dataAcceptThread).detach();
    std::thread(dataSenderThread).detach();

    controlServerThread(&sdr);

    sdr.ShutdownSDR();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}