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

#include <sndfile.h>
#include "crow.h"

#include <sdrplay_api.h>
#include "sdr_handler.hpp"

// =======================================================
// SYSTEM STATE & BUFFER POOLS
// =======================================================
enum class DeviceMode { IDLE, SCANNING, RECORDING, LIVE_LISTEN };
std::atomic<DeviceMode> g_mode(DeviceMode::IDLE);

// THE FIX: Cache the frequency so Live Listen knows what to stream
double g_current_freq = 88.1; 

const int POOL_SIZE = 32;
const int IQ_CHUNK_SIZE = 16384; 
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

SNDFILE* g_wav_file = nullptr;
std::atomic<bool> g_wav_write_active(false);
std::mutex g_wav_mtx;
redisContext* g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

std::mutex g_heartbeat_mtx;
std::condition_variable g_heartbeat_cv;
std::atomic<long> g_heartbeat_count(0);

std::atomic<int> g_diag_usb_samples(0);
std::atomic<int> g_diag_pcm_samples(0);

// =======================================================
// THREAD 1: DONGLE THREAD
// =======================================================
void broadcastAudio(const std::vector<int16_t>& iqBuffer) {
    g_heartbeat_count++;
    g_heartbeat_cv.notify_all();

    if (g_mode == DeviceMode::SCANNING || iqBuffer.empty()) return;

    g_diag_usb_samples.fetch_add(iqBuffer.size() / 2, std::memory_order_relaxed);

    int idx = iq_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
    
    if (iq_pool[idx].in_use.load(std::memory_order_acquire)) {
        static int drop_count = 0;
        if (++drop_count % 50 == 0) std::cerr << "[Dongle] IQ Pool Full! Dropped chunks.\n";
        return; 
    }

    iq_pool[idx].in_use.store(true, std::memory_order_relaxed);
    
    size_t copy_len = std::min(iqBuffer.size(), (size_t)(IQ_CHUNK_SIZE * 2));
    std::memcpy(iq_pool[idx].data.data(), iqBuffer.data(), copy_len * sizeof(int16_t));
    iq_pool[idx].len = copy_len / 2;

    iq_pool[idx].ready.store(true, std::memory_order_release);
    iq_write_idx.fetch_add(1, std::memory_order_relaxed);
    cv_demod.notify_one();
}

// =======================================================
// THREAD 2: DEMOD THREAD (Direct 250kHz Processing)
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
    float g_i_state = 0.0f, g_q_state = 0.0f;
    float g_prev_phase = 0.0f;
    float g_aa_state1 = 0.0f, g_aa_state2 = 0.0f, g_aa_state3 = 0.0f;
    float g_de_state = 0.0f;

    float dc_i_in_prev = 0.0f, dc_q_in_prev = 0.0f;
    float dc_i_out_prev = 0.0f, dc_q_out_prev = 0.0f;
    const float R = 0.995f; 

    const float iq_alpha = 0.2f;   
    const float aa_alpha = 0.05f;
    const float de_alpha = 0.4545f;
    const float FM_GAIN = 0.53f;   
    
    const float resamp_step = 250000.0f / 16000.0f; 
    float g_resamp_index = 0.0f;

    int telemetry_samples = 0;
    float max_rf_amp = 0.0f;

    int pcm_idx = 0;
    size_t out_len = 0;
    bool pcm_acquired = false;

    while (true) {
        int idx = iq_read_idx.load(std::memory_order_relaxed) % POOL_SIZE;
        
        if (!iq_pool[idx].ready.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mtx_demod);
            cv_demod.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }

        if (!pcm_acquired) {
            pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
            if (pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                iq_pool[idx].ready.store(false, std::memory_order_release);
                iq_pool[idx].in_use.store(false, std::memory_order_release);
                iq_read_idx.fetch_add(1, std::memory_order_relaxed);
                continue; 
            }
            pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
            out_len = 0;
            pcm_acquired = true;
        }

        for (size_t i = 0; i < iq_pool[idx].len; i++) {
            float bb_i = static_cast<float>(iq_pool[idx].data[2 * i]);
            float bb_q = static_cast<float>(iq_pool[idx].data[2 * i + 1]);

            float current_amp = std::abs(bb_i) / 32768.0f;
            if (current_amp > max_rf_amp) max_rf_amp = current_amp;
            telemetry_samples++;

            // 1. DC Blocker
            float dc_i_out = bb_i - dc_i_in_prev + R * dc_i_out_prev;
            float dc_q_out = bb_q - dc_q_in_prev + R * dc_q_out_prev;
            
            dc_i_in_prev = bb_i; dc_q_in_prev = bb_q;
            dc_i_out_prev = dc_i_out; dc_q_out_prev = dc_q_out;

            // 2. Digital Low Pass
            g_i_state = (iq_alpha * dc_i_out) + ((1.0f - iq_alpha) * g_i_state);
            g_q_state = (iq_alpha * dc_q_out) + ((1.0f - iq_alpha) * g_q_state);

            // 3. FM Phase Extraction
            float current_phase = fast_atan2(g_q_state, g_i_state);
            float phase_diff = current_phase - g_prev_phase;

            if (phase_diff >  M_PI) phase_diff -= 2.0f * M_PI;
            if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
            g_prev_phase = current_phase;

            // 4. Audio Anti-Aliasing
            g_aa_state1 = (aa_alpha * phase_diff) + ((1.0f - aa_alpha) * g_aa_state1);
            g_aa_state2 = (aa_alpha * g_aa_state1)  + ((1.0f - aa_alpha) * g_aa_state2);
            g_aa_state3 = (aa_alpha * g_aa_state2)  + ((1.0f - aa_alpha) * g_aa_state3);

            // 5. Fractional Resampler
            g_resamp_index += 1.0f;
            if (g_resamp_index >= resamp_step) {
                g_resamp_index -= resamp_step;

                float raw_audio = g_aa_state3 * FM_GAIN;
                g_de_state = (de_alpha * raw_audio) + ((1.0f - de_alpha) * g_de_state);
                float pristine = g_de_state; 

                if (pristine > 1.0f) pristine = 1.0f;
                if (pristine < -1.0f) pristine = -1.0f;

                pcm_pool[pcm_idx].data[out_len++] = static_cast<int16_t>(pristine * 32767.0f);
                
                // THE GAPLESS AUDIO FIX: Flush mid-loop without dropping integers
                if (out_len >= PCM_CHUNK_SIZE - 10) {
                    g_diag_pcm_samples.fetch_add(out_len, std::memory_order_relaxed); 
                    pcm_pool[pcm_idx].len = out_len;
                    pcm_pool[pcm_idx].ready.store(true, std::memory_order_release);
                    pcm_write_idx.fetch_add(1, std::memory_order_relaxed);
                    cv_output.notify_one();
                    
                    pcm_idx = pcm_write_idx.load(std::memory_order_relaxed) % POOL_SIZE;
                    if (pcm_pool[pcm_idx].in_use.load(std::memory_order_acquire)) {
                        pcm_acquired = false;
                        break; 
                    }
                    pcm_pool[pcm_idx].in_use.store(true, std::memory_order_relaxed);
                    out_len = 0;
                }
            }
        }

        iq_pool[idx].ready.store(false, std::memory_order_release);
        iq_pool[idx].in_use.store(false, std::memory_order_release);
        iq_read_idx.fetch_add(1, std::memory_order_relaxed);

        if (telemetry_samples >= 250000) { 
            int iq_pending = iq_write_idx.load() - iq_read_idx.load();
            int pcm_pending = pcm_write_idx.load() - pcm_read_idx.load();
            int current_usb_hz = g_diag_usb_samples.exchange(0);
            int current_pcm_hz = g_diag_pcm_samples.exchange(0);
            
            std::cout << "\n[PIPELINE HEALTH] --- 1 SECOND SNAPSHOT ---" << std::endl;
            std::cout << "  -> USB Ingress: " << current_usb_hz << " Hz (Expected: 250000)" << std::endl;
            std::cout << "  -> PCM Egress : " << current_pcm_hz << " Hz (Expected: 16000)" << std::endl;
            std::cout << "  -> RF Max Amp : " << max_rf_amp << " (Ideal: 0.2 - 0.8)" << std::endl;
            std::cout << "  -> IQ Pool    : " << iq_pending << " / " << POOL_SIZE << " slots active" << std::endl;
            std::cout << "  -> PCM Pool   : " << pcm_pending << " / " << POOL_SIZE << " slots active" << std::endl;
            std::cout << "---------------------------------------------" << std::endl;
            
            telemetry_samples = 0;
            max_rf_amp = 0.0f;
        }
    }
}

// =======================================================
// THREAD 3: OUTPUT THREAD
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
            std::string payload(reinterpret_cast<const char*>(pcm_pool[idx].data.data()), pcm_pool[idx].len * sizeof(int16_t));
            redisReply* r = (redisReply*)redisCommand(local_redis, "PUBLISH live_audio %b", payload.data(), payload.size());
            if (r) freeReplyObject(r);
        }

        pcm_pool[idx].ready.store(false, std::memory_order_release);
        pcm_pool[idx].in_use.store(false, std::memory_order_release);
        pcm_read_idx.fetch_add(1, std::memory_order_relaxed);
    }
}

// =======================================================
// THREAD 4: CONTROLLER THREAD & UTILS
// =======================================================
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
    redisContext *c = redisConnect("ag-redis", 6379);
    redisReply *reply = (redisReply*)redisCommand(c, "SUBSCRIBE sdr_commands");
    if (reply) freeReplyObject(reply);

    while (redisGetReply(c, (void**)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            if (!json) { freeReplyObject(reply); continue; }
            std::string cmd = json["command"].s();

            double targetFreq = g_current_freq; 
            if (json.has("freq")) {
                if (json["freq"].t() == crow::json::type::Number) targetFreq = json["freq"].d();
                else if (json["freq"].t() == crow::json::type::String) targetFreq = std::stod(json["freq"].s());
            }
            g_current_freq = targetFreq; 

            if (cmd == "TUNE" && g_mode != DeviceMode::SCANNING) {
                sdr->TuneFrequency(targetFreq * 1000000.0);
            }
            else if (cmd == "RECORD" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::RECORDING;

                std::thread([sdr, targetFreq]() {
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);
                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        params->rxChannelA->tunerParams.gain.LNAstate = 8;
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
                        params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                            sdrplay_api_Update_Ext1_None);
                    }
                    sdr->TuneFrequency(targetFreq * 1000000.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    std::string filename = "/app/shared/audio/audio.wav";
                    SF_INFO sfinfo;
                    std::memset(&sfinfo, 0, sizeof(sfinfo)); 
                    sfinfo.channels = 1;
                    sfinfo.samplerate = 16000; 
                    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        g_wav_file = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
                        g_wav_write_active = true;
                    }

                    std::cout << "[Recorder] Writing pristine 16kHz audio for " << targetFreq << " MHz..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(30));

                    g_wav_write_active = false;
                    g_mode = DeviceMode::IDLE;

                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        sf_close(g_wav_file);
                        g_wav_file = nullptr;
                    }
                    std::cout << "[Recorder] Complete." << std::endl;

                    crow::json::wvalue msg;
                    msg["event"] = "record_complete";
                    msg["freq"] = targetFreq;
                    msg["file"] = filename;

                    std::lock_guard<std::mutex> r_lock(g_redis_pub_mtx);
                    redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s", msg.dump().c_str());
                    if (r) freeReplyObject(r);
                }).detach();
            }
            else if (cmd == "LIVE_LISTEN" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::LIVE_LISTEN;

                std::thread([sdr, targetFreq]() {
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        params->rxChannelA->tunerParams.gain.LNAstate = 8;
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
                        params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                            sdrplay_api_Update_Ext1_None);
                    }

                    sdr->TuneFrequency(targetFreq * 1000000.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 
                    std::cout << "[Live] Streaming " << targetFreq << " MHz to WebSockets..." << std::endl;
                }).detach();
            }
            else if (cmd == "STOP_LIVE" && g_mode == DeviceMode::LIVE_LISTEN) {
                g_mode = DeviceMode::IDLE;
                std::cout << "[Live] Stopped." << std::endl;
            }
            else if (cmd == "SCAN" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::SCANNING;

                std::thread([sdr]() {
                    std::vector<crow::json::wvalue> found_stations;
                    setBandwidth(sdr, sdrplay_api_BW_0_200, sdrplay_api_IF_0_450);

                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
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

                    double bestFreq = 0.0;
                    float bestRssi = -999.0f;
                    bool inCluster = false;
                    int clusterSteps = 0;

                    for (double freqMHz = 88.1; freqMHz <= 107.9; freqMHz += 0.1) {
                        sdr->TuneFrequency(freqMHz * 1000000.0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(60));

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
                                inCluster = false;
                                bestRssi = -999.0f;
                                bestFreq = 0.0;
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

                    std::cout << "\n[Scanner] === SCAN COMPLETE. FOUND " << found_stations.size() << " STATIONS ===" << std::endl;

                    crow::json::wvalue msg;
                    msg["event"] = "scan_complete";
                    msg["stations"] = std::move(found_stations);
                    {
                        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
                        redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s", msg.dump().c_str());
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

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    g_redis_pub = redisConnect("ag-redis", 6379);

    std::thread(demodThread).detach();
    std::thread(outputThread).detach();

    SdrHandler sdr;
    if (!sdr.InitializeAPI()) return 1;
    if (!sdr.StartStream(88000000.0)) return 1;

    std::cout << "[SDR] Docker Microservice Active. Pipeline Ready." << std::endl;
    commandListener(&sdr);

    return 0;
}