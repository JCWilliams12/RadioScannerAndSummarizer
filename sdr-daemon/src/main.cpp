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
// =======================================================
// DEVICE STATE — independent flags replace the old single-state enum.
// LIVE_LISTEN and RECORDING now coexist. Only SCANNING is exclusive
// (it sweeps frequencies, so nothing else can use the IQ stream).
// =======================================================
std::atomic<bool> g_scanning(false);
std::atomic<bool> g_recording(false);
std::atomic<bool> g_live_listen(false);

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

    if (g_scanning.load(std::memory_order_relaxed) || iqBuffer.empty()) return;

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
// BIQUAD FILTER (standard second-order IIR section)
// =======================================================
struct Biquad {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    double process(double in) {
        double out = b0*in + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        return out;
    }

    void reset() { x1 = x2 = y1 = y2 = 0; }

    // Audio EQ Cookbook: low-pass biquad via bilinear transform
    static Biquad lowPass(double fc, double fs, double Q) {
        double w0    = 2.0 * M_PI * fc / fs;
        double alpha = std::sin(w0) / (2.0 * Q);
        double cosw0 = std::cos(w0);
        double a0    = 1.0 + alpha;

        Biquad bq;
        bq.b0 = ((1.0 - cosw0) / 2.0) / a0;
        bq.b1 =  (1.0 - cosw0)        / a0;
        bq.b2 = ((1.0 - cosw0) / 2.0) / a0;
        bq.a1 = (-2.0 * cosw0)        / a0;
        bq.a2 =  (1.0 - alpha)        / a0;
        return bq;
    }
};


// =======================================================
// THREAD 2: DEMOD THREAD — Clean FM Broadcast DSP
// =======================================================
// With the USB bottleneck eliminated, this is now a textbook
// FM mono demodulator.  No sinc compensators, no notch filters,
// no single-pole hacks.
//
// Pipeline (500 kHz IQ rate from relay with decimation=4):
//   1. Channel filter: 4th-order Butterworth LPF @ 100 kHz
//      Rejects adjacent FM stations (200 kHz spacing)
//   2. FM discriminator: atan2 + phase unwrap
//   3. Audio LPF: 4th-order Butterworth @ 15 kHz
//      Removes 19 kHz pilot, subcarrier, and noise
//   4. De-emphasis: 75 µs (US/Americas standard)
//   5. Fractional resampler: 500 kHz → 16 kHz (linear interp)
//   6. Output: 16 kHz mono PCM for Whisper transcription
// =======================================================
void demodThread() {
    std::cout << "[DEMOD] *** BUILD v13-CLEAN: Butterworth ch@100k + audio@15k, "
              << "75µs de-emph, 500k→16k linear resamp ***" << std::endl;

    // === IQ Sample Rate (must match relay decimation config) ===
    const double IQ_RATE    = 500000.0;   // 2 MHz / 4
    const double AUDIO_RATE = 16000.0;
    const float  RESAMP_RATIO = static_cast<float>(AUDIO_RATE / IQ_RATE);  // 0.032

    // === Channel Filter: 4th-order Butterworth LPF, fc=100 kHz ===
    // Two cascaded biquads per channel (I and Q independently).
    // Butterworth Q values for 4th order: 0.5412 and 1.3066
    Biquad ch_i1 = Biquad::lowPass(100000.0, IQ_RATE, 0.5412);
    Biquad ch_i2 = Biquad::lowPass(100000.0, IQ_RATE, 1.3066);
    Biquad ch_q1 = Biquad::lowPass(100000.0, IQ_RATE, 0.5412);
    Biquad ch_q2 = Biquad::lowPass(100000.0, IQ_RATE, 1.3066);

    // === FM Discriminator State ===
    float prev_phase = 0.0f;

    // === Audio LPF: 4th-order Butterworth, fc=15 kHz ===
    // Runs at the full 500 kHz IQ rate (before decimation) for
    // maximum anti-alias rejection before the resampler.
    Biquad aud1 = Biquad::lowPass(15000.0, IQ_RATE, 0.5412);
    Biquad aud2 = Biquad::lowPass(15000.0, IQ_RATE, 1.3066);

    // === De-emphasis: 75 µs time constant (US standard) ===
    // First-order IIR: y[n] = (1-d)*x[n] + d*y[n-1]
    const double de_d = std::exp(-1.0 / (75.0e-6 * IQ_RATE));   // ≈ 0.9737
    double de_state = 0.0;

    // === FM Gain ===
    // Max phase_diff for ±75 kHz deviation at 500 kHz = ±0.94 rad.
    // After LPF + de-emphasis, speech sits well below that.
    // Gain of 2.0 produces healthy PCM levels without clipping.
    const float FM_GAIN = 2.0f;

    // === Fractional Resampler with Linear Interpolation ===
    float resamp_phase = 0.0f;
    float prev_audio   = 0.0f;   // previous audio sample for interpolation

    int pcm_idx = 0;
    size_t out_len = 0;
    bool pcm_acquired = false;

    auto diag_wall_start = std::chrono::steady_clock::now();

    // === Diagnostic: track effective IQ rate ===
    const int DIAG_IQ_PERIOD = 500000;  // Report every 500k samples (1 sec at 500 kHz)

    while (true) {
        // ---- Gap recovery: reset all filter state ----
        if (g_stream_gap.exchange(false, std::memory_order_acq_rel)) {
            ch_i1.reset(); ch_i2.reset();
            ch_q1.reset(); ch_q2.reset();
            aud1.reset();  aud2.reset();
            de_state = 0.0;
            prev_phase = 0.0f;
            prev_audio = 0.0f;
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

        // Acquire a PCM output slot if we don't have one
        if (!pcm_acquired) {
            pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
            if (!pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
                out_len = 0;
                pcm_acquired = true;
            }
        }

        // ---- Process every IQ sample in this chunk ----
        for (size_t i = 0; i < iq_pool[idx].len; i++) {
            float raw_i = static_cast<float>(iq_pool[idx].data[2 * i]);
            float raw_q = static_cast<float>(iq_pool[idx].data[2 * i + 1]);

            // 1. Channel filter (4th-order Butterworth, fc=100 kHz)
            double fi = ch_i2.process(ch_i1.process(static_cast<double>(raw_i)));
            double fq = ch_q2.process(ch_q1.process(static_cast<double>(raw_q)));

            // 2. FM discriminator: atan2 phase + unwrapped diff
            float current_phase = std::atan2(static_cast<float>(fq), static_cast<float>(fi));
            float phase_diff = current_phase - prev_phase;
            if (phase_diff >  M_PI) phase_diff -= 2.0f * M_PI;
            if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
            prev_phase = current_phase;

            // 3. Audio LPF (4th-order Butterworth, fc=15 kHz)
            double audio = aud2.process(aud1.process(static_cast<double>(phase_diff)));

            // 4. De-emphasis: 75 µs (US standard)
            de_state = (1.0 - de_d) * audio + de_d * de_state;

            float current_audio = static_cast<float>(de_state) * FM_GAIN;

            // 5. Fractional resampler: 500 kHz → 16 kHz with linear interpolation
            resamp_phase += RESAMP_RATIO;
            if (resamp_phase >= 1.0f) {
                resamp_phase -= 1.0f;

                // Linear interpolation between previous and current sample
                float t = resamp_phase / RESAMP_RATIO;
                float sample = prev_audio * (1.0f - t) + current_audio * t;

                // Hard clamp to [-1, 1]
                if (sample >  1.0f) sample =  1.0f;
                if (sample < -1.0f) sample = -1.0f;

                if (pcm_acquired) {
                    pcm_pool[pcm_idx].data[out_len++] = static_cast<int16_t>(sample * 32767.0f);
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
            prev_audio = current_audio;
        }

        // Release the IQ slot
        iq_pool[idx].ready.store(false, std::memory_order_release);
        iq_pool[idx].in_use.store(false, std::memory_order_release);
        iq_read_idx.fetch_add(1, std::memory_order_relaxed);

        auto end_time = std::chrono::high_resolution_clock::now();
        diag_demod_time_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count(), std::memory_order_relaxed);

        // ---- Diagnostics: report every 500k IQ samples (≈1 second) ----
        int current_iq_rx = diag_iq_rx.load();
        if (current_iq_rx >= DIAG_IQ_PERIOD) {
            diag_iq_rx.fetch_sub(DIAG_IQ_PERIOD, std::memory_order_relaxed);

            int d_iq   = diag_iq_drop.exchange(0);
            int d_pgen = diag_pcm_gen.exchange(0);
            int d_pdrp = diag_pcm_drop.exchange(0);
            int d_pwrt = diag_pcm_write.exchange(0);
            long long cpu_us = diag_demod_time_us.exchange(0);
            float cpu_percent = (cpu_us / 1000000.0f) * 100.0f;

            auto diag_wall_now = std::chrono::steady_clock::now();
            float wall_sec = std::chrono::duration<float>(diag_wall_now - diag_wall_start).count();
            diag_wall_start = diag_wall_now;
            float effective_iq_rate = static_cast<float>(DIAG_IQ_PERIOD) / wall_sec;

            std::cout << "\n=============================================" << std::endl;
            std::cout << "[DIAGNOSTICS] --- 1 SECOND REPORT ---" << std::endl;
            std::cout << "  -> Wall Clock    : " << wall_sec << " sec (Expected: ~1.0s)" << std::endl;
            std::cout << "  -> Effective Rate: " << effective_iq_rate << " IQ/sec (Expected: 500000)" << std::endl;
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

        if (g_live_listen.load(std::memory_order_relaxed) && local_redis && !local_redis->err) {
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
                    g_scanning.store(false, std::memory_order_release);

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

            // ---- TUNE: forward to relay (blocked only during scan) ----
            if (cmd == "TUNE" && !g_scanning.load(std::memory_order_relaxed)) {
                crow::json::wvalue relay_cmd;
                relay_cmd["command"] = "TUNE";
                relay_cmd["freq"]    = targetFreq;
                sendCtrlCommand(relay_cmd.dump());
            }
            // ---- RECORD: allowed unless scanning (can coexist with LIVE_LISTEN) ----
            else if (cmd == "RECORD" && !g_scanning.load(std::memory_order_relaxed)
                                     && !g_recording.load(std::memory_order_relaxed)) {
                g_recording.store(true, std::memory_order_release);

                std::thread([targetFreq]() {
                    // Only reconfigure hardware if live_listen isn't already
                    // streaming on the same frequency — avoids a brief glitch.
                    // If live_listen IS active, the hardware is already set up.
                    if (!g_live_listen.load(std::memory_order_relaxed)) {
                        crow::json::wvalue hw_cmd;
                        hw_cmd["command"] = "CONFIGURE";
                        hw_cmd["freq"]    = targetFreq;
                        hw_cmd["bw"]      = "BW_0_300";
                        hw_cmd["if_mode"] = "IF_Zero";
                        hw_cmd["agc"]     = "off";
                        hw_cmd["lna"]     = 4;
                        sendCtrlCommand(hw_cmd.dump());
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    } else {
                        // Already live — just make sure we're on the right freq
                        crow::json::wvalue tune_cmd;
                        tune_cmd["command"] = "TUNE";
                        tune_cmd["freq"]    = targetFreq;
                        sendCtrlCommand(tune_cmd.dump());
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    // Local WAV file management
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
                    g_recording.store(false, std::memory_order_release);

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
            // ---- LIVE_LISTEN: allowed unless scanning (can coexist with RECORD) ----
            else if (cmd == "LIVE_LISTEN" && !g_scanning.load(std::memory_order_relaxed)) {
                if (!freqProvided) {
                    std::cerr << "[LIVE_LISTEN] REJECTED — no freq in command." << std::endl;
                } else if (g_live_listen.load(std::memory_order_relaxed)) {
                    // Already live — just retune if frequency changed
                    crow::json::wvalue tune_cmd;
                    tune_cmd["command"] = "TUNE";
                    tune_cmd["freq"]    = targetFreq;
                    sendCtrlCommand(tune_cmd.dump());
                    std::cout << "[Live] Retuned to " << targetFreq << " MHz." << std::endl;
                } else {
                    g_live_listen.store(true, std::memory_order_release);

                    std::thread([targetFreq]() {
                        // Only reconfigure hardware if not already recording
                        if (!g_recording.load(std::memory_order_relaxed)) {
                            crow::json::wvalue hw_cmd;
                            hw_cmd["command"] = "CONFIGURE";
                            hw_cmd["freq"]    = targetFreq;
                            hw_cmd["bw"]      = "BW_0_300";
                            hw_cmd["if_mode"] = "IF_Zero";
                            hw_cmd["agc"]     = "off";
                            hw_cmd["lna"]     = 4;
                            sendCtrlCommand(hw_cmd.dump());
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        } else {
                            crow::json::wvalue tune_cmd;
                            tune_cmd["command"] = "TUNE";
                            tune_cmd["freq"]    = targetFreq;
                            sendCtrlCommand(tune_cmd.dump());
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        std::cout << "[Live] Streaming " << targetFreq
                                  << " MHz to WebSockets..." << std::endl;
                    }).detach();
                }
            }
            // ---- STOP_LIVE: clear live flag only (recording continues unaffected) ----
            else if (cmd == "STOP_LIVE" && g_live_listen.load(std::memory_order_relaxed)) {
                g_live_listen.store(false, std::memory_order_release);
                std::cout << "[Live] Stopped." << std::endl;
            }
            // ---- SCAN: exclusive — only when nothing else is active ----
            else if (cmd == "SCAN" && !g_scanning.load(std::memory_order_relaxed)
                                   && !g_recording.load(std::memory_order_relaxed)
                                   && !g_live_listen.load(std::memory_order_relaxed)) {
                g_scanning.store(true, std::memory_order_release);
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
// MOCK MODE ROUTER (NO HARDWARE FALLBACK)
// =======================================================
void runMockMode() {
    redisContext *sub = redisConnect("ag-redis", 6379);
    redisContext *pub = redisConnect("ag-redis", 6379);

    if (!sub || sub->err || !pub || pub->err) {
        std::cerr << "[Mock SDR] Failed to connect to Redis." << std::endl;
        return;
    }

    redisReply *reply = (redisReply*)redisCommand(sub, "SUBSCRIBE sdr_commands");
    if (reply) freeReplyObject(reply);

    std::cout << "[Mock SDR] Listening for commands..." << std::endl;

    while (redisGetReply(sub, (void**)&reply) == REDIS_OK) {
        if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            if (!json) continue;

            std::string cmd = json["command"].s();

            if (cmd == "TUNE") {
                double freq = json["freq"].d();
                std::cout << "[Mock SDR] Tuning to " << freq << " MHz (Simulated)" << std::endl;
            }
            else if (cmd == "RECORD") {
                double targetFreq = json["freq"].d();
                std::cout << "[Mock SDR] Recording " << targetFreq << " MHz (Simulating 3 seconds)..." << std::endl;
                
                // Simulate recording delay
                std::this_thread::sleep_for(std::chrono::seconds(3));

                // Copy dummy file into the hot seat for the AI worker
                std::system("cp /app/shared/audio/dummy.wav /app/shared/audio/audio.wav");

                crow::json::wvalue msg;
                msg["event"] = "record_complete";
                msg["freq"] = targetFreq;
                msg["file"] = "/app/shared/audio/audio.wav";
                
                std::cout << "[Mock SDR] Record complete. Alerting AI Worker." << std::endl;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
            else if (cmd == "SCAN") {
                std::cout << "[Mock SDR] Sweeping spectrum (Simulating 4 seconds)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(4));

                crow::json::wvalue msg;
                msg["event"] = "scan_complete";
                msg["stations"][0]["freq"] = 154.280;
                msg["stations"][0]["name"] = "Mock Police Dispatch";
                msg["stations"][1]["freq"] = 462.562;
                msg["stations"][1]["name"] = "Mock Fast Food";
                msg["stations"][2]["freq"] = 162.550;
                msg["stations"][2]["name"] = "Mock NOAA Weather";

                std::cout << "[Mock SDR] Sweep complete. Sending mock stations to React." << std::endl;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
        }
        if (reply) freeReplyObject(reply);
    }
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