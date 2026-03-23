#include <iostream>
#include <thread>
#include <hiredis/hiredis.h>
#include <csignal>
#include <mutex>
#include <vector>
#include <queue>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <complex>

#include <liquid/liquid.h>
#include <sndfile.h>
#include "crow.h"
#include "sdr_handler.hpp"

// =======================================================
// SYSTEM STATE
// =======================================================
enum class DeviceMode { IDLE, SCANNING, RECORDING };
std::atomic<DeviceMode> g_mode(DeviceMode::IDLE);

redisContext* g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

// =======================================================
// THREAD-SAFE QUEUES & BUFFERS
// =======================================================
std::queue<std::vector<int16_t>> g_iq_queue;
std::vector<int16_t> g_iq_flat_buffer; // THE FIX: Thread-safe batching buffer
std::mutex g_iq_mtx;
std::condition_variable g_iq_cv;

std::mutex g_heartbeat_mtx;
std::condition_variable g_heartbeat_cv;
std::atomic<long> g_heartbeat_count(0);

SNDFILE* g_wav_file = nullptr;
std::mutex g_wav_mtx;

// =======================================================
// DSP WORKER THREAD
// =======================================================
void dspWorker() {
    redisContext* local_redis = redisConnect("ag-redis", 6379);
    
    float kf = 75000.0f / 2000000.0f; 
    freqdem demod = freqdem_create(kf);
    
    float resamp_ratio = 48000.0f / 2000000.0f;
    msresamp_rrrf resampler = msresamp_rrrf_create(resamp_ratio, 60.0f);

    while (true) {
        std::vector<int16_t> raw_iq;
        {
            std::unique_lock<std::mutex> lock(g_iq_mtx);
            g_iq_cv.wait(lock, []{ return !g_iq_queue.empty(); });
            raw_iq = std::move(g_iq_queue.front());
            g_iq_queue.pop();
        }

        size_t num_iq_samples = raw_iq.size() / 2;
        std::vector<int16_t> pcm_audio_48khz;
        pcm_audio_48khz.reserve(num_iq_samples * resamp_ratio + 10);

        for (size_t i = 0; i < num_iq_samples; i++) {
            std::complex<float> iq_sample(
                static_cast<float>(raw_iq[2 * i]) / 32768.0f,
                static_cast<float>(raw_iq[2 * i + 1]) / 32768.0f
            );
            
            float audio_float = 0.0f;
            static int dsp_counter = 0;
            if (g_mode == DeviceMode::RECORDING && dsp_counter++ % 500000 == 0) {
                std::cout << "[Probe 2: Demod Float] Out: " << audio_float << std::endl;
            }
            freqdem_demodulate(demod, iq_sample, &audio_float);

            unsigned int num_written = 0;
            // THE FIX: Increased from 4 to 16 to prevent internal liquid-dsp stack smashing
            float resampled_out[16]; 
            msresamp_rrrf_execute(resampler, &audio_float, 1, resampled_out, &num_written);

            for (unsigned int r = 0; r < num_written; r++) {
                float val = resampled_out[r] * 8000.0f; 
                static int pcm_counter = 0;
                if (g_mode == DeviceMode::RECORDING && pcm_counter++ % 48000 == 0) {
                    std::cout << "[Probe 3: PCM Scaled] Val: " << val << " | Int: " << static_cast<int16_t>(val) << std::endl;
                }
                if (val > 32767.0f) val = 32767.0f;
                if (val < -32768.0f) val = -32768.0f;
                pcm_audio_48khz.push_back(static_cast<int16_t>(val));
            }
        }

        if (pcm_audio_48khz.empty()) continue;

        if (g_mode == DeviceMode::RECORDING) {
            std::lock_guard<std::mutex> lock(g_wav_mtx);
            if (g_wav_file) {
                sf_write_short(g_wav_file, pcm_audio_48khz.data(), pcm_audio_48khz.size());
            }
        } 
        else if (g_mode == DeviceMode::IDLE && local_redis) {
            std::string payload(reinterpret_cast<const char*>(pcm_audio_48khz.data()), pcm_audio_48khz.size() * sizeof(int16_t));
            redisReply* r = (redisReply*)redisCommand(local_redis, "PUBLISH live_audio %b", payload.data(), payload.size());
            if (r) freeReplyObject(r);
        }
    }
    freqdem_destroy(demod);
    msresamp_rrrf_destroy(resampler);
}

// =======================================================
// LIGHTWEIGHT USB CALLBACK (MEMORY SAFE)
// =======================================================
void broadcastAudio(const std::vector<int16_t>& iqBuffer) {
    g_heartbeat_count++;
    g_heartbeat_cv.notify_all();

    if (g_mode == DeviceMode::SCANNING || iqBuffer.empty()) return;

    // THE FIX: Move all vector manipulation safely behind the mutex barrier
    // This entirely prevents the `double free or corruption` thread collisions.
    std::lock_guard<std::mutex> lock(g_iq_mtx);
    
    // Append the tiny incoming chunk to our flat buffer
    g_iq_flat_buffer.insert(g_iq_flat_buffer.end(), iqBuffer.begin(), iqBuffer.end());

    // Only dispatch to the heavy DSP thread once we hit a dense 8192-sample chunk
    if (g_iq_flat_buffer.size() >= 8192) {
        if (g_iq_queue.size() > 50) g_iq_queue.pop(); 
        static int raw_counter = 0;
        if (g_mode == DeviceMode::RECORDING && raw_counter++ % 10000 == 0) {
            std::cout << "[Probe 1: Raw IQ] I: " << iqBuffer[0] << " | Q: " << iqBuffer[1] << std::endl;
        }
        g_iq_queue.push(std::move(g_iq_flat_buffer));
        g_iq_flat_buffer = std::vector<int16_t>(); // Create a fresh, safe buffer
        g_iq_flat_buffer.reserve(8192);            // Pre-allocate for speed
        g_iq_cv.notify_one();
    }
}

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

// =======================================================
// COMMAND ROUTER
// =======================================================
void commandListener(SdrHandler* sdr) {
    redisContext *c = redisConnect("ag-redis", 6379);
    redisReply *reply = (redisReply*)redisCommand(c, "SUBSCRIBE sdr_commands");
    if (reply) freeReplyObject(reply);

    while (redisGetReply(c, (void**)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            
            // THE FIX: Protect hiredis from memory leaking unparsed payloads
            if (!json) {
                freeReplyObject(reply);
                continue; 
            }
            
            std::string cmd = json["command"].s();

            if (cmd == "TUNE" && g_mode == DeviceMode::IDLE) {
                double f = json["freq"].d();
                sdr->TuneFrequency(f * 1000000.0);
            } 
            else if (cmd == "RECORD" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::RECORDING;
                double targetFreq = json["freq"].d();
                
                std::thread([sdr, targetFreq]() {
                    // 1. Prepare Hardware
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);
                    sdr->TuneFrequency(targetFreq * 1000000.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 

                    // 2. VERIFY HARDWARE IS ALIVE (The USB Heartbeat Check)
                    float checkStream = getValidatedPower(sdr);
                    if (checkStream <= -99.0f) {
                        std::cerr << "[Recorder] ERROR: USB hardware stalled during tune. Aborting." << std::endl;
                        g_mode = DeviceMode::IDLE;
                        return;
                    }

                    // 3. Open the File
                    std::string filename = "/app/shared/audio.wav";
                    SF_INFO sfinfo;
                    sfinfo.channels = 1;
                    sfinfo.samplerate = 48000;
                    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
                    
                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        g_wav_file = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
                    }

                    if (g_wav_file) {
                        std::cout << "[Recorder] Writing 48kHz audio to " << filename << "..." << std::endl;
                        
                        // THE FIX: We DO NOT call sdr->dspModule->StartRecording() here anymore.
                        // The dspWorker thread is already running globally. 
                        // By setting g_mode to RECORDING, it is actively writing to g_wav_file right now.
                        
                        std::this_thread::sleep_for(std::chrono::seconds(30));
                        
                        // Safely close the file handle
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        sf_close(g_wav_file);
                        g_wav_file = nullptr;
                        
                        crow::json::wvalue msg;
                        msg["event"] = "record_complete"; 
                        msg["freq"] = targetFreq;
                        msg["file"] = filename;
                        
                        std::lock_guard<std::mutex> r_lock(g_redis_pub_mtx);
                        redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s", msg.dump().c_str());
                        if (r) freeReplyObject(r);
                        
                        std::cout << "[Recorder] Recording Finished." << std::endl;
                    } else {
                        std::cerr << "[Recorder] ERROR: Failed to open " << filename << std::endl;
                    }
                    g_mode = DeviceMode::IDLE;
                }).detach();
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
                        params->rxChannelA->tunerParams.gain.LNAstate = 4;
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
                    std::cout << "[Scanner] Floor: " << noiseFloor << " dBFS | Squelch: " << squelchThreshold << " dBFS\n" << std::endl;

                    double bestFreq = 0.0;
                    float bestRssi = -999.0f;
                    bool inCluster = false;
                    int clusterSteps = 0;

                    for (double freqMHz = 88.1; freqMHz <= 107.9; freqMHz += 0.1) {
                        sdr->TuneFrequency(freqMHz * 1000000.0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(60));
                        
                        float currentRssi = getValidatedPower(sdr);

                        // Output restored per your request so we can watch it traverse!
                        std::cout << "[Sweep] TUNE -> " << freqMHz << " MHz | RSSI: " << currentRssi << " dBFS" << std::endl;

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
                                    std::cout << "        >>> LOGGED STATION: " << bestFreq << " MHz <<<" << std::endl;
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
                        std::cout << "        >>> LOGGED STATION: " << bestFreq << " MHz <<<" << std::endl;
                    }

                    sdr->TuneFrequency(88000000.0);
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            sdrplay_api_Update_Ctrl_Agc,
                            sdrplay_api_Update_Ext1_None);
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
        if (reply) freeReplyObject(reply); // Ensures parsed replies are always safely destroyed
    }
    redisFree(c);
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    
    g_redis_pub = redisConnect("ag-redis", 6379);
    std::thread(dspWorker).detach();

    SdrHandler sdr;
    if (!sdr.InitializeAPI()) return 1;
    if (!sdr.StartStream(88000000.0)) return 1;

    std::cout << "[SDR] Docker Microservice Active. Memory protections engaged." << std::endl;
    commandListener(&sdr);

    return 0;
}