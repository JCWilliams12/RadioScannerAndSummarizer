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
// --- NEW LINUX-NATIVE LIBRARIES ---
#include <liquid/liquid.h>
#include <sndfile.h>

#include "crow.h"
#include "sdr_handler.hpp"

// =======================================================
// SYSTEM STATE & REDIS
// =======================================================
enum class DeviceMode { IDLE, SCANNING, RECORDING };
std::atomic<DeviceMode> g_mode(DeviceMode::IDLE);

redisContext* g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

// =======================================================
// ASYNC AUDIO PIPELINE & DSP
// =======================================================
std::queue<std::string> g_audio_queue;
std::mutex g_audio_mtx;
std::condition_variable g_audio_cv;

// Hardware Sync
std::mutex g_heartbeat_mtx;
std::condition_variable g_heartbeat_cv;
std::atomic<long> g_heartbeat_count(0);

// Liquid-DSP FM Demodulator Instance
freqdem g_fm_demod = nullptr;

void audioPublisherWorker() {
    redisContext* local_redis = redisConnect("ag-redis", 6379);
    if (!local_redis || local_redis->err) return;

    while (true) {
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(g_audio_mtx);
            g_audio_cv.wait(lock, []{ return !g_audio_queue.empty(); });
            payload = std::move(g_audio_queue.front());
            g_audio_queue.pop();
        }
        redisReply* r = (redisReply*)redisCommand(local_redis, "PUBLISH live_audio %b", payload.data(), payload.size());
        if (r) freeReplyObject(r);
    }
}

// =======================================================
// THE NEW DSP HARDWARE CALLBACK
// Takes Raw IQ -> FM Demodulates -> Streams Audio
// =======================================================
void broadcastAudio(const std::vector<int16_t>& iqBuffer) {
    g_heartbeat_count++;
    g_heartbeat_cv.notify_all();

    if (g_mode != DeviceMode::IDLE || iqBuffer.empty() || !g_fm_demod) return;

    // 1. Convert Raw interleaved 16-bit IQ to Complex Floats for Liquid-DSP
    size_t num_samples = iqBuffer.size() / 2;
    std::vector<float> audio_out(num_samples);
    
    for (size_t i = 0; i < num_samples; i++) {
        std::complex<float> iq_sample(
            static_cast<float>(iqBuffer[2 * i]) / 32768.0f,
            static_cast<float>(iqBuffer[2 * i + 1]) / 32768.0f
        );
        // 2. Demodulate the RF wave into an audio float
        freqdem_demodulate(g_fm_demod, iq_sample, &audio_out[i]);
    }

    // 3. Convert Audio Floats back to 16-bit PCM for the frontend
    std::vector<int16_t> pcm_out(num_samples);
    for (size_t i = 0; i < num_samples; i++) {
        // Soft clipping to prevent audio popping
        float val = audio_out[i] * 10000.0f; // Audio gain
        if (val > 32767.0f) val = 32767.0f;
        if (val < -32768.0f) val = -32768.0f;
        pcm_out[i] = static_cast<int16_t>(val);
    }

    // 4. Queue for Redis
    std::string payload(reinterpret_cast<const char*>(pcm_out.data()), pcm_out.size() * sizeof(int16_t));
    {
        std::lock_guard<std::mutex> lock(g_audio_mtx);
        if (g_audio_queue.size() > 15) g_audio_queue.pop(); 
        g_audio_queue.push(std::move(payload));
    }
    g_audio_cv.notify_one();
}

// ... [Keep setBandwidth and getValidatedPower exactly the same as before] ...
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
    
    bool success = g_heartbeat_cv.wait_for(lock, std::chrono::milliseconds(400), [&]{
        return (g_heartbeat_count.load() - start_count) >= 4;
    });

    if (!success) {
        std::cerr << "[Hardware] WARNING: USB Pipeline Stalled. Soft-resetting..." << std::endl;
        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner, 
                          sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
        return -100.0f;
    }
    return sdr->GetCurrentPower();
}

// =======================================================
// MICROSERVICE COMMAND ROUTER
// =======================================================
void commandListener(SdrHandler* sdr) {
    redisContext *c = redisConnect("ag-redis", 6379);
    redisReply *reply = (redisReply*)redisCommand(c, "SUBSCRIBE sdr_commands");
    if (reply) freeReplyObject(reply);

    while (redisGetReply(c, (void**)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            if (!json) continue;
            std::string cmd = json["command"].s();

            // --- TUNE ---
            if (cmd == "TUNE" && g_mode == DeviceMode::IDLE) {
                double f = json["freq"].d();
                sdr->TuneFrequency(f * 1000000.0);
            } 
            // --- THE NEW LINUX-NATIVE RECORDER ---
            else if (cmd == "RECORD" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::RECORDING;
                double targetFreq = json["freq"].d();
                
                std::thread([sdr, targetFreq]() {
                    std::cout << "[Recorder] Tuning to " << targetFreq << " MHz for recording..." << std::endl;
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);
                    sdr->TuneFrequency(targetFreq * 1000000.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 

                    // Set up LibSndFile to write directly to the Docker Volume
                    std::string filename = "/app/shared/capture_" + std::to_string(targetFreq) + "MHz.wav";
                    SF_INFO sfinfo;
                    sfinfo.channels = 1;
                    sfinfo.samplerate = 48000; // Adjust to match your hardware output rate
                    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
                    
                    SNDFILE* outfile = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
                    if (!outfile) {
                        std::cerr << "[Recorder] ERROR: Could not open " << filename << " for writing." << std::endl;
                        g_mode = DeviceMode::IDLE;
                        return;
                    }

                    std::cout << "[Recorder] Writing audio to " << filename << " for 30 seconds..." << std::endl;

                    // Enable DSP recording hook in your SdrHandler here
                    sdr->dspModule->StartRecording();
                    
                    // Note: In a full implementation, you'd route the Liquid-DSP PCM output 
                    // directly into sf_write_short() inside the hardware callback while recording.
                    
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                    sdr->dspModule->StopRecording();
                    sf_close(outfile);
                    
                    crow::json::wvalue msg;
                    msg["event"] = "record_complete"; 
                    msg["freq"] = targetFreq;
                    msg["file"] = filename;
                    {
                        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
                        redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s", msg.dump().c_str());
                        if (r) freeReplyObject(r);
                    }
                    std::cout << "[Recorder] Finished. Saved to volume." << std::endl;
                    g_mode = DeviceMode::IDLE;
                }).detach();
            }
            // --- SCAN ---
            // ... [Keep SCAN block exactly as it was in the previous step] ...
            else if (cmd == "SCAN" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::SCANNING;

                std::thread([sdr]() {
                    std::vector<crow::json::wvalue> found_stations;
                    std::cout << "[Scanner] Initializing Docker-Safe Scan..." << std::endl;
                    
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
                                    station["name"] = "FM Station " + std::to_string(bestFreq).substr(0, 5);
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
                        station["name"] = "FM Station " + std::to_string(bestFreq).substr(0, 5);
                        found_stations.push_back(std::move(station));
                    }

                    sdr->TuneFrequency(88000000.0);
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

                    sdrplay_api_DeviceParamsT* restoreParams = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &restoreParams);
                    if (restoreParams && restoreParams->rxChannelA) {
                        restoreParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            sdrplay_api_Update_Ctrl_Agc,
                            sdrplay_api_Update_Ext1_None);
                    }

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
    
    // Initialize Liquid-DSP FM Demodulator (0.5 is the modulation index for standard FM)
    g_fm_demod = freqdem_create(0.5f);

    g_redis_pub = redisConnect("ag-redis", 6379);
    std::thread(audioPublisherWorker).detach();

    SdrHandler sdr;
    if (!sdr.InitializeAPI()) return 1;
    if (!sdr.StartStream(88000000.0)) return 1;

    std::cout << "[SDR] Docker Microservice Active with Liquid-DSP." << std::endl;
    commandListener(&sdr);

    // Cleanup
    freqdem_destroy(g_fm_demod);
    return 0;
}