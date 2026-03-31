#include <iostream>
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

#include <sdrplay_api.h>
#include "sdr_handler.hpp"

// =======================================================
// SYSTEM STATE & BUFFER POOLS
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
// ORIGINAL TELEMETRY TRACKERS
// =======================================================
std::atomic<int>  g_diag_usb_samples(0);
std::atomic<int>  g_diag_pcm_samples(0);
std::atomic<int>  g_diag_drop_count(0);
std::atomic<int>  g_diag_phase_bangs(0);
std::atomic<bool> g_stream_gap(false);

// =======================================================
// NEW LEAK DIAGNOSTIC TRACKERS
// =======================================================
std::atomic<int> diag_iq_rx(0);
std::atomic<int> diag_iq_drop(0);
std::atomic<int> diag_pcm_gen(0);
std::atomic<int> diag_pcm_drop(0);
std::atomic<int> diag_pcm_write(0);
std::atomic<long long> diag_demod_time_us(0);


// =======================================================
// THREAD 1: DONGLE THREAD (Tracks Incoming Radio Waves)
// =======================================================
void broadcastAudio(const std::vector<int16_t>& iqBuffer) {
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
            diag_iq_drop.fetch_add(IQ_CHUNK_SIZE, std::memory_order_relaxed); // LEAK DETECTED!
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
// THREAD 2: DEMOD THREAD (Tracks Math CPU & Dropped Audio)
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
    const float ANTI_DENORM = 1e-15f;
    const double ANTI_DENORM_D = 1e-30;

    float dc_i_in_prev = 0.0f, dc_q_in_prev = 0.0f;
    float dc_i_out_prev = 0.0f, dc_q_out_prev = 0.0f;
    float g_prev_i = 0.0f, g_prev_q = 0.0f;
    float aud_in_prev = 0.0f, aud_out_prev = 0.0f;

    double bq1_x1 = 0.0, bq1_x2 = 0.0, bq1_y1 = 0.0, bq1_y2 = 0.0;
    double bq2_x1 = 0.0, bq2_x2 = 0.0, bq2_y1 = 0.0, bq2_y2 = 0.0;

    // =================================================================
    // ALL COEFFICIENTS BELOW ARE FOR Fs = 31250 Hz (actual throughput)
    // The SDRplay decimation=8 produces properly-filtered samples at
    // 250kHz spacing, but Docker/WSL2 USB delivers only ~31.25k/sec.
    // =================================================================

    // Biquad LPF: 2nd-order Butterworth at 7kHz, Fs=31250
    // (anti-alias for the 31.25kHz → 16kHz resampler)
    const double BQ1_B0 =  0.245030, BQ1_B1 = 0.490060, BQ1_B2 =  0.245030;
    const double BQ1_A1 = -0.198413,                     BQ1_A2 =  0.178524;
    const double BQ2_B0 =  0.245030, BQ2_B1 = 0.490060, BQ2_B2 =  0.245030;
    const double BQ2_A1 = -0.198413,                     BQ2_A2 =  0.178524;

    float g_de_state = 0.0f;
    // 75µs de-emphasis at 31.25kHz: alpha = 1 - exp(-2π × 2122 / 31250)
    const float de_alpha = 0.3475f;
    const float FM_GAIN = 4.0f;   

    // 31250 actual IQ throughput → 16000 PCM output
    const float resamp_ratio = 16000.0f / 31250.0f; // 0.512
    float g_resamp_phase = 0.0f;
    float g_prev_audio = 0.0f;

    int pcm_idx = 0;
    size_t out_len = 0;
    bool pcm_acquired = false;

    // ========== PATCH: Wall-clock timer for rate diagnosis ==========
    auto diag_wall_start = std::chrono::steady_clock::now();

    while (true) {
        if (g_stream_gap.exchange(false, std::memory_order_acq_rel)) {
            dc_i_in_prev = 0.0f; dc_q_in_prev = 0.0f; dc_i_out_prev = 0.0f; dc_q_out_prev = 0.0f;
            g_prev_i = 0.0f; g_prev_q = 0.0f; aud_in_prev = 0.0f; aud_out_prev = 0.0f;
            bq1_x1 = 0.0; bq1_x2 = 0.0; bq1_y1 = 0.0; bq1_y2 = 0.0;
            bq2_x1 = 0.0; bq2_x2 = 0.0; bq2_y1 = 0.0; bq2_y2 = 0.0;
            g_de_state = 0.0f; g_prev_audio = 0.0f;
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

        // START CPU TIMER
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

            float dc_i_out = bb_i - dc_i_in_prev + 0.999f * dc_i_out_prev + ANTI_DENORM;
            float dc_q_out = bb_q - dc_q_in_prev + 0.999f * dc_q_out_prev + ANTI_DENORM;
            dc_i_in_prev = bb_i; dc_q_in_prev = bb_q;
            dc_i_out_prev = dc_i_out; dc_q_out_prev = dc_q_out;

            float i_conj = (dc_i_out * g_prev_i) + (dc_q_out * g_prev_q);
            float q_conj = (dc_q_out * g_prev_i) - (dc_i_out * g_prev_q);
            g_prev_i = dc_i_out; g_prev_q = dc_q_out;
            float phase_diff = fast_atan2(q_conj, i_conj);

            float aud_out = phase_diff - aud_in_prev + 0.999f * aud_out_prev + ANTI_DENORM;
            aud_in_prev = phase_diff; aud_out_prev = aud_out;

            double d_aud = static_cast<double>(aud_out) + ANTI_DENORM_D;
            double bq1_out = BQ1_B0 * d_aud + BQ1_B1 * bq1_x1 + BQ1_B2 * bq1_x2 - BQ1_A1 * bq1_y1 - BQ1_A2 * bq1_y2;
            bq1_x2 = bq1_x1; bq1_x1 = d_aud; bq1_y2 = bq1_y1; bq1_y1 = bq1_out;

            double aa_out = BQ2_B0 * bq1_out + BQ2_B1 * bq2_x1 + BQ2_B2 * bq2_x2 - BQ2_A1 * bq2_y1 - BQ2_A2 * bq2_y2;
            bq2_x2 = bq2_x1; bq2_x1 = bq1_out; bq2_y2 = bq2_y1; bq2_y1 = aa_out;

            float final_audio = static_cast<float>(aa_out);
            g_de_state = (de_alpha * final_audio) + ((1.0f - de_alpha) * g_de_state) + ANTI_DENORM;

            g_resamp_phase += resamp_ratio;
            if (g_resamp_phase >= 1.0f) {
                g_resamp_phase -= 1.0f;
                float mu = g_resamp_phase / resamp_ratio; 
                float interpolated_audio = (g_prev_audio * mu) + (g_de_state * (1.0f - mu));
                float pristine = std::tanh(interpolated_audio * FM_GAIN);

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
                    diag_pcm_drop.fetch_add(1, std::memory_order_relaxed); // LEAK DETECTED!
                    
                    pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
                    if (!pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                        pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
                        out_len = 0;
                        pcm_acquired = true;
                    }
                }
            }
            g_prev_audio = g_de_state; 
        }

        iq_pool[idx].ready.store(false, std::memory_order_release);
        iq_pool[idx].in_use.store(false, std::memory_order_release);
        iq_read_idx.fetch_add(1, std::memory_order_relaxed);

        // STOP CPU TIMER
        auto end_time = std::chrono::high_resolution_clock::now();
        diag_demod_time_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count(), std::memory_order_relaxed);

        // PRINT REPORT EVERY 250,000 SAMPLES
        int current_iq_rx = diag_iq_rx.load();
        if (current_iq_rx >= 250000) {
            diag_iq_rx.fetch_sub(250000, std::memory_order_relaxed); // Reset counter

            int d_iq   = diag_iq_drop.exchange(0);
            int d_pgen = diag_pcm_gen.exchange(0);
            int d_pdrp = diag_pcm_drop.exchange(0);
            int d_pwrt = diag_pcm_write.exchange(0);
            long long cpu_us = diag_demod_time_us.exchange(0);
            float cpu_percent = (cpu_us / 1000000.0f) * 100.0f;

            // ========== PATCH: Wall-clock elapsed time ==========
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
// THREAD 3: OUTPUT THREAD (Disk Writer + Diagnostic Tracker)
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

        // Correctly writes directly to the WSL2 Disk
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

        // Track the successfully written frames!
        diag_pcm_write.fetch_add(pcm_pool[idx].len, std::memory_order_relaxed);

        pcm_pool[idx].ready.store(false, std::memory_order_release);
        pcm_pool[idx].in_use.store(false, std::memory_order_release);
        pcm_read_idx.fetch_add(1, std::memory_order_relaxed);
    }
}

// =======================================================
// THREAD 4: CONTROLLER THREAD & UTILS
// =======================================================

// ========== PATCH: Re-assert decimation after every BW/IF change ==========
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

    // Re-assert decimation=8 (see StartStream comment for rationale)
    params->rxChannelA->ctrlParams.decimation.enable          = 1;
    params->rxChannelA->ctrlParams.decimation.decimationFactor = 8;
    params->rxChannelA->ctrlParams.decimation.wideBandSignal   = 0;
    sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
        sdrplay_api_Update_Ctrl_Decimation, sdrplay_api_Update_Ext1_None);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "[HW] setBandwidth complete — decimation=8 re-asserted" << std::endl;
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

void commandListener(SdrHandler* sdr) {
    redisContext* c = redisConnect("ag-redis", 6379);
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

            if (cmd == "TUNE" && g_mode != DeviceMode::SCANNING) {
                sdr->TuneFrequency(targetFreq * 1000000.0);
            }
            else if (cmd == "RECORD" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::RECORDING;

                std::thread([sdr, targetFreq]() {
                    // BW_0_200: 200kHz hardware passband sufficient for mono FM
                    // (needs ±90kHz). Previously BW_1_536 (1.536MHz) was allowing
                    // adjacent FM stations to alias into the baseband.
                    setBandwidth(sdr, sdrplay_api_BW_0_200, sdrplay_api_IF_Zero);

                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        params->rxChannelA->tunerParams.gain.LNAstate = 4;
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                            sdrplay_api_Update_Ext1_None);
                    }

                    sdr->TuneFrequency(targetFreq * 1000000.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

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

                    std::cout << "[Recorder] Writing 16kHz audio for " << targetFreq << " MHz..." << std::endl;
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
                    redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s",
                                                              msg.dump().c_str());
                    if (r) freeReplyObject(r);
                }).detach();
            }
            else if (cmd == "LIVE_LISTEN" && g_mode == DeviceMode::IDLE) {
                if (!freqProvided) {
                    std::cerr << "[LIVE_LISTEN] REJECTED — no freq in command. "
                              << "API must include {\"freq\": <MHz>} in the Redis message." << std::endl;
                } else {
                    g_mode = DeviceMode::LIVE_LISTEN;

                    std::thread([sdr, targetFreq]() {
                        // BW_0_200: same rationale as RECORD above.
                        setBandwidth(sdr, sdrplay_api_BW_0_200, sdrplay_api_IF_Zero);

                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        params->rxChannelA->tunerParams.gain.LNAstate = 4;
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                            sdrplay_api_Update_Ext1_None);
                    }

                        sdr->TuneFrequency(targetFreq * 1000000.0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        std::cout << "[Live] Streaming " << targetFreq << " MHz to WebSockets..." << std::endl;
                    }).detach();
                }
            }
            else if (cmd == "STOP_LIVE" && g_mode == DeviceMode::LIVE_LISTEN) {
                g_mode = DeviceMode::IDLE;
                std::cout << "[Live] Stopped." << std::endl;
            }
            else if (cmd == "SCAN" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::SCANNING;

                std::thread([sdr]() {
                    std::vector<crow::json::wvalue> found_stations;
                    // FIX: IF_0_450 permanently corrupts the hardware decimation state
                    // on RSPdx when switching back to IF_Zero mid-stream. The API's
                    // Update_Ctrl_Decimation call is silently ignored while streaming.
                    // IF_Zero works fine for power-based scanning — any DC offset is
                    // negligible relative to a real FM carrier's broadband energy.
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
                    float squelchThreshold = noiseFloor + 12.0f;

                    std::cout << "\n[Scanner] === WIDEBAND SCAN INITIATED ===" << std::endl;

                    double bestFreq     = 0.0;
                    float  bestRssi     = -999.0f;
                    bool   inCluster    = false;
                    int    clusterSteps = 0;

                    for (double freqMHz = 88.1; freqMHz <= 107.9; freqMHz += 0.1) {
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
                                    crow::json::wvalue station;
                                    station["freq"] = bestFreq;
                                    station["name"] = "FM " + std::to_string(bestFreq).substr(0, 5);
                                    found_stations.push_back(std::move(station));
                                }
                                inCluster    = false;
                                bestRssi     = -999.0f;
                                bestFreq     = 0.0;
                                clusterSteps = 0;
                            }
                        }
                    }

                    if (inCluster && clusterSteps >= 2 && bestFreq > 0.0) {
                        crow::json::wvalue station;
                        station["freq"] = bestFreq;
                        station["name"] = "FM " + std::to_string(bestFreq).substr(0, 5);
                        found_stations.push_back(std::move(station));
                    }

                    std::cout << "\n[Scanner] === SCAN COMPLETE. FOUND "
                              << found_stations.size() << " STATIONS ===" << std::endl;

                    crow::json::wvalue msg;
                    msg["event"]    = "scan_complete";
                    msg["stations"] = std::move(found_stations);
                    {
                        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
                        redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s",
                                                                  msg.dump().c_str());
                        if (r) freeReplyObject(r);
                    }

                    g_mode = DeviceMode::IDLE;
                }).detach();
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

    g_redis_pub = redisConnect("ag-redis", 6379);

    std::thread demod_t(demodThread);
    {
        struct sched_param sp;
        sp.sched_priority = 10;
        int ret = pthread_setschedparam(demod_t.native_handle(), SCHED_FIFO, &sp);
        if (ret != 0) {
            std::cerr << "[WARN] Could not set SCHED_FIFO for demod thread (errno=" << ret
                      << "). Ensure 'privileged: true' is set in docker-compose.yml." << std::endl;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(demod_t.native_handle(), sizeof(cpu_set_t), &cpuset);
    }
    demod_t.detach();

    std::thread(outputThread).detach();

    SdrHandler sdr;
    if (!sdr.InitializeAPI()) return 1;
    if (!sdr.StartStream(88000000.0)) return 1;

    std::cout << "[SDR] Docker Microservice Active. Pipeline Ready." << std::endl;
    commandListener(&sdr);

    return 0;
}