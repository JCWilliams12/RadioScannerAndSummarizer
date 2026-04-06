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

#include <sdrplay_api.h>
#include "sdr_handler.hpp"

// =======================================================
// SYSTEM STATE
// =======================================================
enum class DeviceMode { IDLE, SCANNING, RECORDING, LIVE_LISTEN };
std::atomic<DeviceMode> g_mode(DeviceMode::IDLE);

std::atomic<int> g_dropped_chunks(0);
std::atomic<int> g_processed_chunks(0);

redisContext* g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

std::queue<std::vector<int16_t>> g_iq_queue;
std::vector<int16_t> g_iq_flat_buffer;
std::mutex g_iq_mtx;
std::condition_variable g_iq_cv;

std::queue<std::vector<int16_t>> g_pcm_queue;
std::mutex g_pcm_mtx;
std::condition_variable g_pcm_cv;

std::mutex g_heartbeat_mtx;
std::condition_variable g_heartbeat_cv;
std::atomic<long> g_heartbeat_count(0);

SNDFILE* g_wav_file = nullptr;
std::mutex g_wav_mtx;
std::atomic<bool> g_wav_write_active(false);

// =======================================================
// REDIS PUBLISH THREAD
// =======================================================
void redisPublishWorker() {
    redisContext* local_redis = redisConnect("ag-redis", 6379);
    if (!local_redis || local_redis->err) return;

    while (true) {
        std::vector<int16_t> pcm;
        {
            std::unique_lock<std::mutex> lock(g_pcm_mtx);
            g_pcm_cv.wait(lock, []{ return !g_pcm_queue.empty(); });
            pcm = std::move(g_pcm_queue.front());
            g_pcm_queue.pop();
        }

        if (pcm.empty()) continue;

        std::string payload(reinterpret_cast<const char*>(pcm.data()), pcm.size() * sizeof(int16_t));
        redisReply* r = (redisReply*)redisCommand(local_redis, "PUBLISH live_audio %b", payload.data(), payload.size());
        if (r) freeReplyObject(r);
    }
}

// =======================================================
// DSP WORKER (Hybrid: Custom Phase Math + Liquid Resampler)
// =======================================================
void dspWorker() {
    // 1. Resample precisely from 250kHz down to 16kHz
    float resamp_ratio = 16000.0f / 250000.0f;
    msresamp_rrrf resampler = msresamp_rrrf_create(resamp_ratio, 60.0f);

    // 2. State variables from your old working code
    float i_state = 0.0f, q_state = 0.0f;
    float prev_phase = 0.0f;
    float aa_state1 = 0.0f, aa_state2 = 0.0f, aa_state3 = 0.0f;
    float de_state = 0.0f;

    // 3. Adjusted Math for 250kHz physics
    const float iq_alpha = 0.5f;   // Light smoothing
    const float aa_alpha = 0.2f;   // Audio anti-aliasing
    const float de_alpha = 0.4545f;
    const float FM_GAIN  = 0.53f;  // 4.25 divided by 8 to prevent 250kHz phase clipping!

    while (true) {
        std::vector<int16_t> raw_iq;
        {
            std::unique_lock<std::mutex> lock(g_iq_mtx);
            g_iq_cv.wait(lock, []{ return !g_iq_queue.empty(); });
            raw_iq = std::move(g_iq_queue.front());
            g_iq_queue.pop();
        }
        g_iq_cv.notify_all();

        size_t numSamples = raw_iq.size() / 2;
        std::vector<float> audio_demod(numSamples);

        for (unsigned int i = 0; i < numSamples; i++) {
            float bb_i = static_cast<float>(raw_iq[2 * i]);
            float bb_q = static_cast<float>(raw_iq[2 * i + 1]);

            // 1. Digital Channel Filter
            i_state = (iq_alpha * bb_i) + ((1.0f - iq_alpha) * i_state);
            q_state = (iq_alpha * bb_q) + ((1.0f - iq_alpha) * q_state);

            // 2. Custom FM Demodulation (Now running safely at 250kHz!)
            float current_phase = std::atan2(q_state, i_state);
            float phase_diff = current_phase - prev_phase;

            if (phase_diff >  M_PI) phase_diff -= 2.0f * M_PI;
            if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
            prev_phase = current_phase;

            // 3. Audio Smoothing
            aa_state1 = (aa_alpha * phase_diff) + ((1.0f - aa_alpha) * aa_state1);
            aa_state2 = (aa_alpha * aa_state1)  + ((1.0f - aa_alpha) * aa_state2);
            aa_state3 = (aa_alpha * aa_state2)  + ((1.0f - aa_alpha) * aa_state3);

            audio_demod[i] = aa_state3 * FM_GAIN;
        }

        // 4. Liquid-DSP Resampler: Precisely convert 250,000 array to 16,000 array
        unsigned int max_resamp_out = std::ceil(numSamples * resamp_ratio) + 32;
        std::vector<float> resamp_out(max_resamp_out);
        unsigned int num_written = 0;
        msresamp_rrrf_execute(resampler, audio_demod.data(), numSamples, resamp_out.data(), &num_written);

        // 5. De-emphasis and PCM Scaling
        std::vector<int16_t> pcm_audio_16khz;
        pcm_audio_16khz.reserve(num_written);
        
        for (unsigned int i = 0; i < num_written; i++) {
            float raw_audio = resamp_out[i];
            
            de_state = (de_alpha * raw_audio) + ((1.0f - de_alpha) * de_state);
            float pristine_float = de_state; 
            
            // Hard clamp to prevent wrapping distortion
            if (pristine_float > 1.0f) pristine_float = 1.0f;
            if (pristine_float < -1.0f) pristine_float = -1.0f;

            float live_int = pristine_float * 32767.0f;
            if (live_int >  32767.0f) live_int =  32767.0f;
            if (live_int < -32768.0f) live_int = -32768.0f;
            
            pcm_audio_16khz.push_back(static_cast<int16_t>(live_int));
        }

        g_processed_chunks++;

        if (pcm_audio_16khz.empty()) continue;

        if (g_wav_write_active.load()) {
            std::lock_guard<std::mutex> lock(g_wav_mtx);
            if (g_wav_file) {
                sf_write_short(g_wav_file, pcm_audio_16khz.data(), pcm_audio_16khz.size());
            }
        }

        if (g_mode == DeviceMode::LIVE_LISTEN) {
            {
                std::lock_guard<std::mutex> lock(g_pcm_mtx);
                g_pcm_queue.push(pcm_audio_16khz);
            }
            g_pcm_cv.notify_one();
        }
    }
}

// =======================================================
// LIGHTWEIGHT IQ CALLBACK
// =======================================================
void broadcastAudio(const std::vector<int16_t>& iqBuffer) {
    g_heartbeat_count++;
    g_heartbeat_cv.notify_all();

    if (g_mode == DeviceMode::SCANNING || iqBuffer.empty()) return;

    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(g_iq_mtx);
        g_iq_flat_buffer.insert(g_iq_flat_buffer.end(), iqBuffer.begin(), iqBuffer.end());

        if (g_iq_flat_buffer.size() >= 16384) {
            if (g_iq_queue.size() >= 50) {
                g_iq_flat_buffer.clear();
                g_iq_flat_buffer.reserve(16384);
                return;
            }
            g_iq_queue.push(std::move(g_iq_flat_buffer));
            g_iq_flat_buffer = std::vector<int16_t>();
            g_iq_flat_buffer.reserve(16384);
            should_notify = true;
        }
    }
    if (should_notify) g_iq_cv.notify_one();
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
            if (!json) { freeReplyObject(reply); continue; }
            std::string cmd = json["command"].s();

            if (cmd == "TUNE" && g_mode != DeviceMode::SCANNING) {
                double f = json["freq"].d();
                sdr->TuneFrequency(f * 1000000.0);
            }
            else if (cmd == "RECORD" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::RECORDING;
                double targetFreq = json["freq"].d();

                std::thread([sdr, targetFreq]() {
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        // Inherit the exact LNA state the old scanner used!
                        params->rxChannelA->tunerParams.gain.LNAstate = 4;
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
                    sfinfo.channels = 1;
                    sfinfo.samplerate = 16000;
                    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        g_wav_file = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
                    }

                    if (g_wav_file) {
                        g_wav_write_active = true;

                        std::cout << "[Recorder] Writing clean 48kHz audio to " << filename << "..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(30));

                        g_mode = DeviceMode::IDLE;

                        {
                            std::lock_guard<std::mutex> lock(g_iq_mtx);
                            if (!g_iq_flat_buffer.empty()) {
                                g_iq_queue.push(std::move(g_iq_flat_buffer));
                                g_iq_flat_buffer.clear();
                                g_iq_flat_buffer.reserve(16384);
                            }
                        }
                        g_iq_cv.notify_one();

                        {
                            std::unique_lock<std::mutex> lock(g_iq_mtx);
                            g_iq_cv.wait(lock, []{ return g_iq_queue.empty(); });
                        }

                        g_wav_write_active = false;
                        {
                            std::lock_guard<std::mutex> lock(g_wav_mtx);
                            sf_close(g_wav_file);
                            g_wav_file = nullptr;
                        }

                        std::cout << "[Recorder] Finished writing 30 seconds." << std::endl;

                        crow::json::wvalue msg;
                        msg["event"] = "record_complete";
                        msg["freq"] = targetFreq;
                        msg["file"] = filename;

                        std::lock_guard<std::mutex> r_lock(g_redis_pub_mtx);
                        redisReply* r = (redisReply*)redisCommand(g_redis_pub, "PUBLISH ws_updates %s", msg.dump().c_str());
                        if (r) freeReplyObject(r);
                    } else {
                        g_mode = DeviceMode::IDLE;
                    }
                }).detach();
            }
            else if (cmd == "LIVE_LISTEN" && g_mode == DeviceMode::IDLE) {
                g_mode = DeviceMode::LIVE_LISTEN;

                // 1. EXTRACT FREQUENCY: Actually grab the station from the React JSON
                double targetFreq = json.has("freq") ? json["freq"].d() : 88.1;

                std::thread([sdr, targetFreq]() {
                    // 2. DODGE THE SPIKE: Push the DC electrical offset 450kHz away
                    setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

                    sdrplay_api_DeviceParamsT* params = nullptr;
                    sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
                    if (params && params->rxChannelA) {
                        // Inherit the exact LNA state the old scanner used!
                        params->rxChannelA->tunerParams.gain.LNAstate = 4;
                        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
                        params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;
                        sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                            (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                            sdrplay_api_Update_Ext1_None);
                    }

                    // 4. TUNE: Actually command the hardware to change to the station!
                    sdr->TuneFrequency(targetFreq * 1000000.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Let the tuner settle

                    // 5. YOUR STABLE WAV LOGIC
                    std::string filename = "/app/shared/audio/live.wav";
                    SF_INFO sfinfo;
                    sfinfo.channels = 1; sfinfo.samplerate = 16000; sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
                    {
                        std::lock_guard<std::mutex> lock(g_wav_mtx);
                        g_wav_file = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
                        g_wav_write_active = true;
                    }
                    std::cout << "[Live] Streaming " << targetFreq << " MHz to Redis AND saving to " << filename << std::endl;
                }).detach();
            }
            else if (cmd == "STOP_LIVE" && g_mode == DeviceMode::LIVE_LISTEN) {
                g_mode = DeviceMode::IDLE;
                g_wav_write_active = false;
                {
                    std::lock_guard<std::mutex> lock(g_wav_mtx);
                    if (g_wav_file) {
                        sf_close(g_wav_file);
                        g_wav_file = nullptr;
                    }
                }
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

int main() {
    std::signal(SIGPIPE, SIG_IGN);
<<<<<<< HEAD

    g_redis_pub = redisConnect("ag-redis", 6379);

    std::thread(dspWorker).detach();
    std::thread(redisPublishWorker).detach();

=======
    g_redis_pub = redisConnect("ag-redis", 6379);
    
    std::cout << "[SDR] Attempting to initialize hardware..." << std::endl;
>>>>>>> 1e5ab748bb8441ba63521c633fa58c18256b0cbf
    SdrHandler sdr;

<<<<<<< HEAD
    std::cout << "[SDR] Docker Microservice Active. Pipeline Ready." << std::endl;
=======
    // IF HARDWARE FAILS -> BOOT INTO MOCK MODE
    if (!sdr.InitializeAPI()) {
        std::cout << "[WARNING] No SDRplay devices found!" << std::endl;
        std::cout << "[WARNING] Booting into MOCK MODE for UI/AI testing." << std::endl;
        runMockMode();
        return 0; // Exit cleanly when Mock Mode shuts down
    }

    // IF HARDWARE SUCCEEDS -> BOOT INTO LIVE MODE
    if (!sdr.StartStream(88000000.0)) {
        std::cerr << "[SDR] Failed to start stream." << std::endl;
        return 1;
    }

    std::cout << "[SDR] Hardware found! Docker Microservice Active." << std::endl;
    std::thread(dspWorker).detach(); // Start DSP only if hardware is live
>>>>>>> 1e5ab748bb8441ba63521c633fa58c18256b0cbf
    commandListener(&sdr);

    return 0;
}