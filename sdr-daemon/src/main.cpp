#include <iostream>
#define _USE_MATH_DEFINES
#include <thread>
#include <hiredis/hiredis.h>
#include <csignal>
#include <vector>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <sched.h>
#include <pthread.h>

#include <sndfile.h>
#include "crow.h"

// TCP networking (replaces sdrplay_api.h and sdr_handler.hpp)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>

// =======================================================
// IQ-OVER-TCP WIRE PROTOCOL
// =======================================================
// The sdr-relay streams on the DATA port (default 7373):
//   [uint32_t num_samples (network order)] [int16_t I, Q, I, Q, ...]
//
// The CONTROL port (default 7374) carries newline-delimited JSON:
//   sdr-daemon  → relay:  {"command":"TUNE","freq":88.1}
//   relay → sdr-daemon:   {"event":"scan_complete","stations":[...]}
// =======================================================

static const uint32_t IQ_HEADER_MAGIC = 0x49515043; // "IQPC"

// =======================================================
// SYSTEM STATE & BUFFER POOLS  (UNCHANGED from original)
// =======================================================
enum class DeviceMode { IDLE, SCANNING, RECORDING, LIVE_LISTEN };
std::atomic<DeviceMode> g_mode(DeviceMode::IDLE);

double g_current_freq = 88.1;

const int POOL_SIZE      = 32;
const int IQ_CHUNK_SIZE  = 16384;
const int PCM_CHUNK_SIZE = 2048;

struct IQSlot {
    std::array<int16_t, IQ_CHUNK_SIZE * 2> data;
    size_t len;
    std::atomic<bool> in_use{false};
    std::atomic<bool> ready{false};
} iq_pool[POOL_SIZE];

struct PCMSlot {
    std::array<int16_t, PCM_CHUNK_SIZE> data;
    size_t len;
    std::atomic<bool> in_use{false};
    std::atomic<bool> ready{false};
} pcm_pool[POOL_SIZE];

std::atomic<int> iq_write_idx(0), iq_read_idx(0);
std::atomic<int> pcm_write_idx(0), pcm_read_idx(0);

std::condition_variable cv_demod;
std::mutex mtx_demod;

std::condition_variable cv_output;
std::mutex mtx_output;

SNDFILE* g_wav_file         = nullptr;
std::atomic<bool>  g_wav_write_active(false);
std::mutex         g_wav_mtx;
redisContext* g_redis_pub        = nullptr;
std::mutex         g_redis_pub_mtx;

std::mutex              g_heartbeat_mtx;
std::condition_variable g_heartbeat_cv;
std::atomic<long>       g_heartbeat_count(0);

// =======================================================
// ORIGINAL TELEMETRY TRACKERS  (UNCHANGED)
// =======================================================
std::atomic<int>  g_diag_usb_samples(0);
std::atomic<int>  g_diag_pcm_samples(0);
std::atomic<int>  g_diag_drop_count(0);
std::atomic<int>  g_diag_phase_bangs(0);
std::atomic<bool> g_stream_gap(false);

// =======================================================
// LEAK DIAGNOSTIC TRACKERS  (UNCHANGED)
// =======================================================
std::atomic<int> diag_iq_rx(0);
std::atomic<int> diag_iq_drop(0);
std::atomic<int> diag_pcm_gen(0);
std::atomic<int> diag_pcm_drop(0);
std::atomic<int> diag_pcm_write(0);
std::atomic<long long> diag_demod_time_us(0);

// =======================================================
// TCP RELAY CONNECTION STATE
// =======================================================
std::atomic<int> g_ctrl_fd(-1);
std::mutex       g_ctrl_mtx;

// =======================================================
// TCP HELPERS
// =======================================================
static int connectToRelay(const char* host, int port) {
    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    // Disable Nagle for low-latency IQ streaming
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    return fd;
}

// Read exactly n bytes from a socket (handles partial reads)
static bool recvExact(int fd, void* buf, size_t n) {
    size_t total = 0;
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (total < n) {
        ssize_t r = recv(fd, p + total, n - total, 0);
        if (r <= 0) return false;  // disconnect or error
        total += r;
    }
    return true;
}

// Send a newline-delimited JSON command on the control socket
static bool sendCtrlCommand(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(g_ctrl_mtx);
    int fd = g_ctrl_fd.load(std::memory_order_relaxed);
    if (fd < 0) return false;

    std::string msg = json_str + "\n";
    ssize_t sent = send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
    return sent == (ssize_t)msg.size();
}


// =======================================================
// IQ INGEST (replaces broadcastAudio — same ring buffer logic)
// =======================================================
// This function is identical to the original broadcastAudio().
// It takes interleaved IQ samples and feeds them into iq_pool[].
// The ONLY difference is who calls it: previously the SDRplay USB
// callback, now the TCP reader thread.
// =======================================================
void feedIQToPool(const std::vector<int16_t>& iqBuffer) {
    if (++g_heartbeat_count % 100 == 0) {
        g_heartbeat_cv.notify_all();
    }

    if (g_mode == DeviceMode::SCANNING || iqBuffer.empty()) return;

    static std::vector<int16_t> staging_buf(IQ_CHUNK_SIZE * 2, 0);
    static size_t staging_len  = 0;

    size_t offset = 0;
    const size_t incoming = iqBuffer.size();
    diag_iq_rx.fetch_add(incoming / 2, std::memory_order_relaxed);

    while (offset < incoming) {
        size_t space   = (size_t)(IQ_CHUNK_SIZE * 2) - staging_len;
        size_t to_copy = std::min(space, incoming - offset);

        std::memcpy(staging_buf.data() + staging_len,
                    iqBuffer.data()    + offset,
                    to_copy * sizeof(int16_t));

        staging_len += to_copy;
        offset      += to_copy;

        if (staging_len < (size_t)(IQ_CHUNK_SIZE * 2)) break;

        int idx = iq_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;

        if (iq_pool[idx].in_use.load(std::memory_order_acquire)) {
            g_stream_gap.store(true, std::memory_order_release);
            diag_iq_drop.fetch_add(IQ_CHUNK_SIZE, std::memory_order_relaxed);
        } else {
            iq_pool[idx].in_use.store(true, std::memory_order_relaxed);
            std::memcpy(iq_pool[idx].data.data(), staging_buf.data(), IQ_CHUNK_SIZE * 2 * sizeof(int16_t));
            iq_pool[idx].len = IQ_CHUNK_SIZE;
            iq_pool[idx].ready.store(true, std::memory_order_release);
            iq_write_idx.fetch_add(1, std::memory_order_relaxed);
            cv_demod.notify_one();
        }
        staging_len = 0;
    }
}


// =======================================================
// THREAD 1: TCP READER (replaces USB Dongle callback)
// =======================================================
// Connects to the sdr-relay's DATA port, reads framed IQ chunks,
// and pushes them into the ring buffer via feedIQToPool().
//
// Wire format per chunk:
//   [4 bytes: magic 0x49515043]
//   [4 bytes: num_samples as uint32_t, network byte order]
//   [num_samples * 2 * sizeof(int16_t) bytes: interleaved I,Q]
// =======================================================
void tcpReaderThread(const char* host, int port) {
    std::vector<int16_t> recv_buf;
    recv_buf.reserve(65536);

    while (true) {
        std::cout << "[TCP-IQ] Connecting to relay at " << host << ":" << port << "..." << std::endl;
        int fd = connectToRelay(host, port);
        if (fd < 0) {
            std::cerr << "[TCP-IQ] Connection failed, retrying in 2s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Increase receive buffer for bursty IQ traffic
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        std::cout << "[TCP-IQ] Connected to relay data stream." << std::endl;

        while (true) {
            // Read frame header: magic + num_samples
            uint32_t header[2];
            if (!recvExact(fd, header, sizeof(header))) {
                std::cerr << "[TCP-IQ] Disconnected from relay (header read)." << std::endl;
                g_stream_gap.store(true, std::memory_order_release);
                break;
            }

            uint32_t magic       = ntohl(header[0]);
            uint32_t num_samples = ntohl(header[1]);

            if (magic != IQ_HEADER_MAGIC) {
                std::cerr << "[TCP-IQ] Bad magic 0x" << std::hex << magic << std::dec
                          << " — resyncing by reconnecting." << std::endl;
                g_stream_gap.store(true, std::memory_order_release);
                break;
            }

            if (num_samples == 0 || num_samples > 65536) {
                std::cerr << "[TCP-IQ] Invalid sample count " << num_samples << std::endl;
                break;
            }

            // Read IQ payload: num_samples * 2 int16_t values (I, Q interleaved)
            size_t payload_ints = num_samples * 2;
            recv_buf.resize(payload_ints);

            if (!recvExact(fd, recv_buf.data(), payload_ints * sizeof(int16_t))) {
                std::cerr << "[TCP-IQ] Disconnected from relay (payload read)." << std::endl;
                g_stream_gap.store(true, std::memory_order_release);
                break;
            }

            // Feed directly into the DSP ring buffer
            feedIQToPool(recv_buf);
        }

        close(fd);
        std::cout << "[TCP-IQ] Will reconnect in 2s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}


// =======================================================
// THREAD 2: DEMOD THREAD  (100% UNCHANGED from original)
// =======================================================
inline float fast_atan2(float y, float x) {
    float abs_y = std::abs(y) + 1e-10f;
    float angle;
    if (x >= 0) {
        float r = (x - abs_y) / (x + abs_y);
        angle = 0.78539816f - 0.78539816f * r;
    } else {
        float r = (x + abs_y) / (abs_y - x);
        angle = 2.35619449f - 0.78539816f * r;
    }
    return y < 0 ? -angle : angle;
}

void demodThread() {
    std::cout << "[DEMOD] *** BUILD v12-TCP: SINC_COMPENSATOR EQ@1800Hz +15dB, de=0.1, notch Q=15 ***" << std::endl;

    float i_state = 0.0f;
    float q_state = 0.0f;
    const float iq_alpha = 0.8f;

    float prev_phase = 0.0f;

    float aa_state1 = 0.0f;
    float aa_state2 = 0.0f;
    float aa_state3 = 0.0f;
    const float aa_alpha = 0.08f;

    // Notch filter at 2375 Hz: Q=15 (sharp, ~160Hz width)
    const double w0_notch = 2.0 * M_PI * 2375.0 / 16000.0;
    const double alpha_notch = std::sin(w0_notch) / (2.0 * 15.0);
    const double NB0 = 1.0 / (1.0 + alpha_notch);
    const double NB1 = (-2.0 * std::cos(w0_notch)) / (1.0 + alpha_notch);
    const double NB2 = 1.0 / (1.0 + alpha_notch);
    const double NA1 = NB1;
    const double NA2 = (1.0 - alpha_notch) / (1.0 + alpha_notch);
    double nx1 = 0.0, nx2 = 0.0, ny1 = 0.0, ny2 = 0.0;

    // Peaking EQ: +15 dB at 1800 Hz, Q=1.5, Fs=16000
    const double PB0 = 1.3880, PB1 = -1.3996, PB2 = 0.4442;
    const double PA1 = -1.3996, PA2 = 0.8322;
    double px1 = 0.0, px2 = 0.0, py1 = 0.0, py2 = 0.0;

    float de_state = 0.0f;
    const float de_alpha = 0.56f;
    const float FM_GAIN = 4.25f;

    const float resamp_ratio = 16000.0f / 125000.0f;
    float g_resamp_phase = 0.0f;
    float g_prev_audio = 0.0f;

    int pcm_idx = 0;
    size_t out_len = 0;
    bool pcm_acquired = false;

    auto diag_wall_start = std::chrono::steady_clock::now();

    while (true) {
        if (g_stream_gap.exchange(false, std::memory_order_acq_rel)) {
            i_state = 0.0f; q_state = 0.0f;
            prev_phase = 0.0f;
            aa_state1 = 0.0f; aa_state2 = 0.0f; aa_state3 = 0.0f;
            de_state = 0.0f; g_prev_audio = 0.0f;
            nx1 = 0.0; nx2 = 0.0; ny1 = 0.0; ny2 = 0.0;
            px1 = 0.0; px2 = 0.0; py1 = 0.0; py2 = 0.0;
        }

        int idx = iq_read_idx.load(std::memory_order_relaxed) % POOL_SIZE;

        if (!iq_pool[idx].ready.load(std::memory_order_acquire)) {
            for (int spin = 0; spin < 1000; ++spin) {
                if (iq_pool[idx].ready.load(std::memory_order_acquire)) goto have_data;
                #if defined(__x86_64__) || defined(__i386__)
                    __asm__ volatile("pause" ::: "memory");
                #endif
            }
            std::this_thread::yield();
            continue;
        }
        have_data:;

        auto start_time = std::chrono::high_resolution_clock::now();

        if (!pcm_acquired) {
            pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
            if (!pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
                out_len = 0;
                pcm_acquired = true;
            }
        }

        for (size_t i = 0; i < iq_pool[idx].len; i++) {
            float bb_i = static_cast<float>(iq_pool[idx].data[2 * i]);
            float bb_q = static_cast<float>(iq_pool[idx].data[2 * i + 1]);

            i_state = (iq_alpha * bb_i) + ((1.0f - iq_alpha) * i_state);
            q_state = (iq_alpha * bb_q) + ((1.0f - iq_alpha) * q_state);

            float current_phase = std::atan2(q_state, i_state);
            float phase_diff = current_phase - prev_phase;
            if (phase_diff >  M_PI) phase_diff -= 2.0f * M_PI;
            if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
            prev_phase = current_phase;

            aa_state1 = (aa_alpha * phase_diff) + ((1.0f - aa_alpha) * aa_state1);
            aa_state2 = (aa_alpha * aa_state1)  + ((1.0f - aa_alpha) * aa_state2);
            aa_state3 = (aa_alpha * aa_state2)  + ((1.0f - aa_alpha) * aa_state3);

            g_resamp_phase += resamp_ratio;
            if (g_resamp_phase >= 1.0f) {
                g_resamp_phase -= 1.0f;

                float raw_audio = aa_state3 * FM_GAIN;
                de_state = (de_alpha * raw_audio) + ((1.0f - de_alpha) * de_state);

                double notch_in = static_cast<double>(de_state);
                double notch_out = NB0 * notch_in + NB1 * nx1 + NB2 * nx2 - NA1 * ny1 - NA2 * ny2;
                nx2 = nx1; nx1 = notch_in; ny2 = ny1; ny1 = notch_out;

                double eq_out = PB0 * notch_out + PB1 * px1 + PB2 * px2 - PA1 * py1 - PA2 * py2;
                px2 = px1; px1 = notch_out; py2 = py1; py1 = eq_out;

                float pristine = static_cast<float>(eq_out);
                if (pristine >  1.0f) pristine =  1.0f;
                if (pristine < -1.0f) pristine = -1.0f;

                if (pcm_acquired) {
                    pcm_pool[pcm_idx].data[out_len++] = static_cast<int16_t>(pristine * 32767.0f);
                    if (out_len >= PCM_CHUNK_SIZE - 10) {
                        diag_pcm_gen.fetch_add(out_len, std::memory_order_relaxed);
                        pcm_pool[pcm_idx].len = out_len;
                        pcm_pool[pcm_idx].ready.store(true, std::memory_order_release);
                        pcm_write_idx.fetch_add(1, std::memory_order_relaxed);
                        cv_output.notify_one();

                        pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
                        if (pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                            pcm_acquired = false;
                        } else {
                            pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
                            out_len = 0;
                        }
                    }
                } else {
                    diag_pcm_drop.fetch_add(1, std::memory_order_relaxed);

                    pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
                    if (!pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                        pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
                        out_len = 0;
                        pcm_acquired = true;
                    }
                }
            }
        }

        iq_pool[idx].ready.store(false, std::memory_order_release);
        iq_pool[idx].in_use.store(false, std::memory_order_release);
        iq_read_idx.fetch_add(1, std::memory_order_relaxed);

        auto end_time = std::chrono::high_resolution_clock::now();
        diag_demod_time_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count(), std::memory_order_relaxed);

        int current_iq_rx = diag_iq_rx.load();
        if (current_iq_rx >= 250000) {
            diag_iq_rx.fetch_sub(250000, std::memory_order_relaxed);

            int d_iq   = diag_iq_drop.exchange(0);
            int d_pgen = diag_pcm_gen.exchange(0);
            int d_pdrp = diag_pcm_drop.exchange(0);
            int d_pwrt = diag_pcm_write.exchange(0);
            long long cpu_us = diag_demod_time_us.exchange(0);
            float cpu_percent = (cpu_us / 1000000.0f) * 100.0f;

            auto diag_wall_now = std::chrono::steady_clock::now();
            float wall_sec = std::chrono::duration<float>(diag_wall_now - diag_wall_start).count();
            diag_wall_start = diag_wall_now;
            float effective_iq_rate = 250000.0f / wall_sec;

            std::cout << "\n=============================================" << std::endl;
            std::cout << "[DIAGNOSTICS] --- 1 SECOND LEAK REPORT ---" << std::endl;
            std::cout << "  -> Wall Clock    : " << wall_sec << " sec (Expected: ~1.0s)" << std::endl;
            std::cout << "  -> Effective Rate: " << effective_iq_rate << " IQ/sec (Expected: 250000)" << std::endl;
            std::cout << "  -> Math CPU Load : " << cpu_percent << "%" << std::endl;
            std::cout << "  -> IQ Dropped    : " << d_iq << " samples (If > 0: CPU is too slow)" << std::endl;
            std::cout << "  -> Audio Gen     : " << d_pgen << " samples" << std::endl;
            std::cout << "  -> Audio Dropped : " << d_pdrp << " samples (If > 0: Output thread/Redis is blocking)" << std::endl;
            std::cout << "  -> Audio Written : " << d_pwrt << " samples" << std::endl;
            std::cout << "=============================================\n" << std::endl;
        }
    }
}


// =======================================================
// THREAD 3: OUTPUT THREAD  (100% UNCHANGED from original)
// =======================================================
void outputThread() {
    redisContext* local_redis = redisConnect("ag-redis", 6379);

    while (true) {
        int idx = pcm_read_idx.load(std::memory_order_relaxed) % POOL_SIZE;

        if (!pcm_pool[idx].ready.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mtx_output);
            cv_output.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }

        if (g_wav_write_active.load()) {
            std::lock_guard<std::mutex> lock(g_wav_mtx);
            if (g_wav_file) {
                sf_write_short(g_wav_file, pcm_pool[idx].data.data(), pcm_pool[idx].len);
            }
        }

        if (g_mode == DeviceMode::LIVE_LISTEN && local_redis && !local_redis->err) {
            std::string payload(reinterpret_cast<const char*>(pcm_pool[idx].data.data()),
                                pcm_pool[idx].len * sizeof(int16_t));
            redisReply* r = (redisReply*)redisCommand(local_redis, "PUBLISH live_audio %b",
                                                      payload.data(), payload.size());
            if (r) freeReplyObject(r);
        }

        diag_pcm_write.fetch_add(pcm_pool[idx].len, std::memory_order_relaxed);

        pcm_pool[idx].ready.store(false, std::memory_order_release);
        pcm_pool[idx].in_use.store(false, std::memory_order_release);
        pcm_read_idx.fetch_add(1, std::memory_order_relaxed);
    }
}


// =======================================================
// THREAD 4: CONTROL SOCKET READER
// =======================================================
// Reads newline-delimited JSON events from the relay's control port
// and publishes them to Redis so the existing api-core/frontend
// pipeline works without any changes.
// =======================================================
void ctrlReaderThread(const char* host, int port) {
    while (true) {
        std::cout << "[TCP-CTRL] Connecting to relay control at "
                  << host << ":" << port << "..." << std::endl;

        int fd = connectToRelay(host, port);
        if (fd < 0) {
            std::cerr << "[TCP-CTRL] Connection failed, retrying in 2s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        g_ctrl_fd.store(fd, std::memory_order_release);
        std::cout << "[TCP-CTRL] Connected to relay control channel." << std::endl;

        // Line-buffered reader
        std::string line_buf;
        char chunk[4096];

        while (true) {
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;

            line_buf.append(chunk, n);

            // Process complete lines
            size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                std::string line = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);

                if (line.empty()) continue;

                auto json = crow::json::load(line);
                if (!json) {
                    std::cerr << "[TCP-CTRL] Bad JSON from relay: " << line << std::endl;
                    continue;
                }

                std::string event = json.has("event") ? std::string(json["event"].s()) : "";

                // ---- scan_complete: relay finished scanning, forward to Redis ----
                if (event == "scan_complete") {
                    std::cout << "[TCP-CTRL] Received scan_complete from relay." << std::endl;
                    g_mode = DeviceMode::IDLE;

                    std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
                    redisReply* r = (redisReply*)redisCommand(
                        g_redis_pub, "PUBLISH ws_updates %s", line.c_str());
                    if (r) freeReplyObject(r);
                }
                // ---- configure_ack: relay confirmed hardware settings ----
                else if (event == "configure_ack") {
                    std::cout << "[TCP-CTRL] Relay confirmed hardware config." << std::endl;
                }
                // ---- Forward any other relay events to Redis ----
                else if (!event.empty()) {
                    std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
                    redisReply* r = (redisReply*)redisCommand(
                        g_redis_pub, "PUBLISH ws_updates %s", line.c_str());
                    if (r) freeReplyObject(r);
                }
            }
        }

        std::cerr << "[TCP-CTRL] Disconnected from relay control." << std::endl;
        g_ctrl_fd.store(-1, std::memory_order_release);
        close(fd);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}


// =======================================================
// THREAD 5: REDIS COMMAND LISTENER
// =======================================================
// Subscribes to Redis sdr_commands (same as before) but instead
// of calling SdrHandler methods, forwards commands to the relay
// over the TCP control socket.  Local responsibilities like WAV
// recording and mode management stay here.
// =======================================================
void commandListener() {
    redisContext* c = redisConnect("ag-redis", 6379);
    if (!c || c->err) {
        std::cerr << "[CMD] Redis connection failed!" << std::endl;
        return;
    }
    redisReply* reply = (redisReply*)redisCommand(c, "SUBSCRIBE sdr_commands");
    if (reply) freeReplyObject(reply);

    while (redisGetReply(c, (void**)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            if (!json) { freeReplyObject(reply); continue; }

            std::string cmd = json["command"].s();

            bool   freqProvided = false;
            double targetFreq   = g_current_freq;

            if (json.has("freq")) {
                freqProvided = true;
                if (json["freq"].t() == crow::json::type::Number)
                    targetFreq = json["freq"].d();
                else if (json["freq"].t() == crow::json::type::String)
                    targetFreq = std::stod(json["freq"].s());
            }

            std::cout << "[CMD] Received: cmd=" << cmd
                      << " freq=" << (freqProvided
                            ? std::to_string(targetFreq)
                            : "NOT PROVIDED (fallback: " + std::to_string(g_current_freq) + ")")
                      << std::endl;

            g_current_freq = targetFreq;

            // ---- TUNE: forward to relay ----
            if (cmd == "TUNE" && g_mode != DeviceMode::SCANNING) {
                crow::json::wvalue relay_cmd;
                relay_cmd["command"] = "TUNE";
                relay_cmd["freq"]    = targetFreq;
                sendCtrlCommand(relay_cmd.dump());
            }
            // ---- RECORD: configure relay hardware, manage local WAV recording ----
            else if (cmd == "RECORD" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::RECORDING;

                std::thread([targetFreq]() {
                    // Tell relay to configure hardware for recording
                    crow::json::wvalue hw_cmd;
                    hw_cmd["command"] = "CONFIGURE";
                    hw_cmd["freq"]    = targetFreq;
                    hw_cmd["bw"]      = "BW_0_200";
                    hw_cmd["if_mode"] = "IF_Zero";
                    hw_cmd["agc"]     = "off";
                    hw_cmd["lna"]     = 4;
                    sendCtrlCommand(hw_cmd.dump());

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    // Local WAV file management (unchanged)
                    std::string filename = "/app/shared/audio/audio.wav";
                    SF_INFO sfinfo;
                    std::memset(&sfinfo, 0, sizeof(sfinfo));
                    sfinfo.channels   = 1;
                    sfinfo.samplerate = 16000;
                    sfinfo.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        g_wav_file         = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
                        g_wav_write_active = true;
                    }

                    std::cout << "[Recorder] Writing 16kHz audio for "
                              << targetFreq << " MHz..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(30));

                    g_wav_write_active = false;
                    g_mode             = DeviceMode::IDLE;

                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        sf_close(g_wav_file);
                        g_wav_file = nullptr;
                    }
                    std::cout << "[Recorder] Complete." << std::endl;

                    crow::json::wvalue msg;
                    msg["event"] = "record_complete";
                    msg["freq"]  = targetFreq;
                    msg["file"]  = filename;

                    std::lock_guard<std::mutex> r_lock(g_redis_pub_mtx);
                    redisReply* r = (redisReply*)redisCommand(
                        g_redis_pub, "PUBLISH ws_updates %s", msg.dump().c_str());
                    if (r) freeReplyObject(r);
                }).detach();
            }
            // ---- LIVE_LISTEN: configure relay, set local mode ----
            else if (cmd == "LIVE_LISTEN" && g_mode == DeviceMode::IDLE) {
                if (!freqProvided) {
                    std::cerr << "[LIVE_LISTEN] REJECTED — no freq in command." << std::endl;
                } else {
                    g_mode = DeviceMode::LIVE_LISTEN;

                    std::thread([targetFreq]() {
                        crow::json::wvalue hw_cmd;
                        hw_cmd["command"] = "CONFIGURE";
                        hw_cmd["freq"]    = targetFreq;
                        hw_cmd["bw"]      = "BW_0_200";
                        hw_cmd["if_mode"] = "IF_Zero";
                        hw_cmd["agc"]     = "off";
                        hw_cmd["lna"]     = 4;
                        sendCtrlCommand(hw_cmd.dump());

                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        std::cout << "[Live] Streaming " << targetFreq
                                  << " MHz to WebSockets..." << std::endl;
                    }).detach();
                }
            }
            // ---- STOP_LIVE: local mode change only ----
            else if (cmd == "STOP_LIVE" && g_mode == DeviceMode::LIVE_LISTEN) {
                g_mode = DeviceMode::IDLE;
                std::cout << "[Live] Stopped." << std::endl;
            }
            // ---- SCAN: delegate entirely to relay ----
            else if (cmd == "SCAN" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::SCANNING;
                crow::json::wvalue relay_cmd;
                relay_cmd["command"] = "SCAN";
                sendCtrlCommand(relay_cmd.dump());
                std::cout << "[SCAN] Delegated to relay." << std::endl;
                // ctrlReaderThread will receive scan_complete and publish to Redis
            }
        }
        if (reply) freeReplyObject(reply);
    }
    redisFree(c);
}


// =======================================================
// MAIN
// =======================================================
int main() {
    std::signal(SIGPIPE, SIG_IGN);

    // Read relay connection info from environment
    const char* relay_host_env = std::getenv("SDR_RELAY_HOST");
    const char* relay_data_env = std::getenv("SDR_RELAY_DATA_PORT");
    const char* relay_ctrl_env = std::getenv("SDR_RELAY_CTRL_PORT");

    std::string relay_host = relay_host_env ? relay_host_env : "host.docker.internal";
    int relay_data_port    = relay_data_env ? std::atoi(relay_data_env) : 7373;
    int relay_ctrl_port    = relay_ctrl_env ? std::atoi(relay_ctrl_env) : 7374;

    std::cout << "\n[SDR-DAEMON] ====== TCP-MODE (no USB) ======" << std::endl;
    std::cout << "  Relay Host : " << relay_host << std::endl;
    std::cout << "  Data Port  : " << relay_data_port << std::endl;
    std::cout << "  Ctrl Port  : " << relay_ctrl_port << std::endl;
    std::cout << "=============================================\n" << std::endl;

    // Connect to Redis
    g_redis_pub = redisConnect("ag-redis", 6379);
    if (!g_redis_pub || g_redis_pub->err) {
        std::cerr << "[SDR-DAEMON] Redis connection failed!" << std::endl;
        return 1;
    }

    // Start demod thread with RT priority (unchanged)
    std::thread demod_t(demodThread);
    {
        struct sched_param sp;
        sp.sched_priority = 10;
        int ret = pthread_setschedparam(demod_t.native_handle(), SCHED_FIFO, &sp);
        if (ret != 0) {
            std::cerr << "[WARN] Could not set SCHED_FIFO for demod thread (errno="
                      << ret << ")." << std::endl;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(demod_t.native_handle(), sizeof(cpu_set_t), &cpuset);
    }
    demod_t.detach();

    // Start output thread (unchanged)
    std::thread(outputThread).detach();

    // Start TCP control channel reader (receives events from relay)
    std::thread(ctrlReaderThread, relay_host.c_str(), relay_ctrl_port).detach();

    // Start TCP IQ data reader (replaces USB callback as IQ source)
    std::thread(tcpReaderThread, relay_host.c_str(), relay_data_port).detach();

    std::cout << "[SDR-DAEMON] Pipeline active. Waiting for relay connection..." << std::endl;

    // Block on Redis command listener (same as original blocking on commandListener)
    commandListener();

    return 0;
}