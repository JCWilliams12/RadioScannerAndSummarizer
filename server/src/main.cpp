#include <iostream>
#include <string>
#include <stdio.h>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <curl/curl.h>

// Project Header Includes
#include "dbcorefunctions.hpp"
#include "dbcorefilter.hpp"
#include "crow.h"
#include "ollamatest.hpp"
#include "whispertinytest.hpp"
#include "sdr_handler.hpp"
#include "openai.hpp"

// Standard Library Includes
#include <fstream>
#include <sstream>

// =======================================================
// WEBSOCKET GLOBAL STATE (The Live Audio Y-Splitter)
// =======================================================
std::mutex ws_mtx;
std::unordered_set<crow::websocket::connection*> connected_clients;

void broadcastAudio(const std::vector<int16_t>& audioBuffer) {
    std::lock_guard<std::mutex> lock(ws_mtx);
    if (connected_clients.empty() || audioBuffer.empty()) return;
    std::string payload(reinterpret_cast<const char*>(audioBuffer.data()), audioBuffer.size() * sizeof(int16_t));
    for (auto client : connected_clients) {
        client->send_binary(payload);
    }
}

// =======================================================
// CORS PREFLIGHT HELPER
// =======================================================
crow::response makeCorsPreflightResponse() {
    crow::response res(204);
    res.add_header("Access-Control-Allow-Origin", "*");
    res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    return res;
}

// =======================================================
// BANDWIDTH SWITCH HELPER
// Scan mode:  BW_0_200 + IF_0_450 — isolates individual FM channels
// Live mode:  BW_1_536 + IF_1_620 — full wideband for best audio quality
// Must pass BOTH BwType AND IfType update flags or hardware ignores one.
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

void openFrontEnd(SdrHandler* sdr) {

    crow::SimpleApp app;

    curl_global_init(CURL_GLOBAL_ALL);
    auto env = loadEnv(".env");
    std::string apiKey = "";
    if (env.find("OPENAI_API_KEY") != env.end()) {
        apiKey = env["OPENAI_API_KEY"];
    } else {
        std::cerr << "CRITICAL ERROR: OPENAI_API_KEY not found in .env file!" << std::endl;
    }
    // create DB 
    createTable();
    
    // =======================================================
    // WEBSOCKET ROUTE: LIVE AUDIO STREAM
    // =======================================================
    CROW_WEBSOCKET_ROUTE(app, "/ws/audio")
    .onopen([&](crow::websocket::connection& conn) {
        std::lock_guard<std::mutex> lock(ws_mtx);
        connected_clients.insert(&conn);
        std::cout << "[WebSocket] Live audio connection established." << std::endl;
    })
    .onclose([&](crow::websocket::connection& conn, const std::string& reason) {
        std::lock_guard<std::mutex> lock(ws_mtx);
        connected_clients.erase(&conn);
        std::cout << "[WebSocket] Connection closed." << std::endl;
    });

    // =======================================================
    // EXPLICIT CORS PREFLIGHT ROUTES
    // =======================================================
    CROW_ROUTE(app, "/api/scan/record").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/scan/tune").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/scan/wideband").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/logs/save").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/logs/delete").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/transcribe/openai").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/summarize/openai").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/transcribe/local").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    CROW_ROUTE(app, "/api/summarize/local").methods(crow::HTTPMethod::Options)
    ([](const crow::request&) { return makeCorsPreflightResponse(); });

    // =======================================================
    // ROUTE: TRIGGER 30-SECOND RECORDING
    // =======================================================
    CROW_ROUTE(app, "/api/scan/record")
    .methods(crow::HTTPMethod::Post)
    ([&sdr](const crow::request& req) {
        auto json_data = crow::json::load(req.body);
        if (!json_data) {
            std::cerr << "[Record] Failed to parse JSON body!" << std::endl;
            crow::response err_res(400, "Invalid JSON");
            err_res.add_header("Access-Control-Allow-Origin", "*");
            return err_res;
        }

        double targetFreq = json_data["freq"].d();
        std::cout << "[Hardware] Async Record Request for " << targetFreq << " MHz" << std::endl;

        std::thread([sdr, targetFreq]() {
            // Guarantee wideband mode is active before recording.
            // If a sweep ran recently, hardware may still be settling
            // from BW_0_200/IF_0_450 back to BW_1_536/IF_1_620.
            // The IqToWav demodulator expects IF_1_620 — wrong IF = silence.
            setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

            sdr->TuneFrequency(targetFreq * 1000000.0);

            // 500ms settle: PLL lock (50ms) + IF filter settle (150ms) + margin
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            sdr->dspModule->StartRecording();
            std::this_thread::sleep_for(std::chrono::seconds(30));
            sdr->dspModule->StopRecording();
        }).detach();

        crow::json::wvalue res_body;
        res_body["status"] = "recording_started";
        crow::response final_res(res_body);
        final_res.add_header("Access-Control-Allow-Origin", "*");
        return final_res;
    });

    // =======================================================
    // ROUTE: GET AVAILABLE STATIONS
    // =======================================================
    CROW_ROUTE(app, "/stations")([]() {
        std::vector<crow::json::wvalue> station_list;
        crow::json::wvalue json_data(station_list);
        crow::response res(json_data);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/api/rds/live")
    ([&sdr]() {
        // This will block the HTTP request for 3.5 seconds while it listens to the live wave
        std::string rdsName = sdr->GetLiveRdsText();
        
        crow::json::wvalue res;
        res["name"] = rdsName;
        return res;
    });

    // =======================================================
    // ROUTE: WIDEBAND SPECTRUM SCANNER (88.0 - 108.0 MHz)
    // =======================================================
    CROW_ROUTE(app, "/api/scan/wideband")
    ([&sdr]() {
        std::vector<crow::json::wvalue> found_stations;

        // Switch to narrow bandwidth for accurate per-channel power readings.
        // BW_0_200 + IF_0_450 is the valid RSPdx combo — IF_1_620 caused
        // ~0.2 MHz frequency offset, IF_Zero caused DC spike corruption.
        std::cout << "[Scanner] Switching to narrow bandwidth (200 kHz / IF_0_450)..." << std::endl;
        setBandwidth(sdr, sdrplay_api_BW_0_200, sdrplay_api_IF_0_450);

        // Disable AGC so gain doesn't compensate for static during sweep
        sdrplay_api_DeviceParamsT* params = nullptr;
        sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &params);
        if (params && params->rxChannelA) {
            params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
            params->rxChannelA->tunerParams.gain.LNAstate = 4;
            sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr),
                sdrplay_api_Update_Ext1_None);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Calibrate noise floor on dead air
        sdr->TuneFrequency(87.9 * 1000000.0);
        sdr->ClearPowerHistory();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        float noiseFloor = sdr->GetCurrentPower();
        float squelchThreshold = noiseFloor + 12.0f;

        std::cout << "[Scanner] Noise Floor: " << noiseFloor << " dBFS" << std::endl;
        std::cout << "[Scanner] Squelch: " << squelchThreshold << " dBFS" << std::endl;

        double bestFreq     = 0.0;
        float  bestRssi     = -999.0f;
        bool   inCluster    = false;
        int    clusterSteps = 0;

        // 1. Scan by 0.2 MHz on the odd decimal
        for (double freqMHz = 88.1; freqMHz <= 107.9; freqMHz += 0.1) {
            sdr->TuneFrequency(freqMHz * 1000000.0);
            
            // 1. Give the hardware PLL 40ms to lock onto the new frequency
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            sdr->ClearPowerHistory();
            
            // 2. Accumulate exactly 40ms of clean RF data to prevent -100 dBFS empty buffers
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            float currentRssi = sdr->GetCurrentPower();
            std::cout << "[Sweep] " << freqMHz << " MHz -> " << currentRssi << " dBFS" << std::endl;

            // --- THE CLUSTER LOGIC ---
            if (currentRssi >= squelchThreshold) {
                inCluster = true;
                clusterSteps++;
                
                // Track the absolute physical peak regardless of odd/even rules
                if (currentRssi > bestRssi) {
                    bestRssi = currentRssi;
                    bestFreq = freqMHz;
                }
            } else {
                if (inCluster) {
                    if (clusterSteps >= 2 && bestFreq > 0.0) {
                        
                        // ==========================================================
                        // THE POST-CLUSTER FCC SNAP
                        // ==========================================================
                        // Convert the peak frequency to an integer (e.g., 102.6 -> 1026)
                        int freq10 = static_cast<int>(std::round(bestFreq * 10.0));
                        
                        // If the physical peak was an even decimal due to atmospheric fading, snap it!
                        if (freq10 % 2 == 0) {
                            freq10 -= 1; // Snap down to the valid odd decimal (102.6 -> 102.5)
                            bestFreq = freq10 / 10.0;
                        }
                        // ==========================================================

                        crow::json::wvalue station;
                        station["freq"] = bestFreq;
                    
                        station["name"] = "FM Station " + std::to_string(bestFreq).substr(0, 5);

                        found_stations.push_back(std::move(station));
                        
                        std::cout << "[Scanner] STATION LOGGED: " << bestFreq 
                                  << " MHz | Peak: " << bestRssi 
                                  << " dBFS | Width: " << clusterSteps << " steps" << std::endl;
                    } else {
                        std::cout << "[Scanner] Rejected noise spike (" << clusterSteps << " step)" << std::endl;
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
            station["name"] = "FM Station " + std::to_string(bestFreq).substr(0, 5);
            found_stations.push_back(std::move(station));
            std::cout << "[Scanner] STATION LOGGED: " << bestFreq << " MHz (End of Band)" << std::endl;
        }

        // Restore wideband mode for live audio and recording
        std::cout << "[Scanner] Restoring wideband mode (1.536 MHz / IF_Zero)..." << std::endl;
        setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero);

        sdrplay_api_DeviceParamsT* restoreParams = nullptr;
        sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &restoreParams);
        if (restoreParams && restoreParams->rxChannelA) {
            restoreParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
            sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                sdrplay_api_Update_Ctrl_Agc,
                sdrplay_api_Update_Ext1_None);
        }

        std::cout << "[Scanner] Sweep complete. Found " << found_stations.size() << " stations." << std::endl;

        crow::json::wvalue res;
        res["stations"] = std::move(found_stations);
        crow::response response(res);
        response.add_header("Access-Control-Allow-Origin", "*");
        return response;
    });

    // =======================================================
    // ROUTE: DYNAMIC HARDWARE TUNING
    // =======================================================
    CROW_ROUTE(app, "/api/scan/tune").methods(crow::HTTPMethod::Post)
    ([&sdr](const crow::request& req) {
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Bad JSON");

        double freqMHz = x.has("freq") ? x["freq"].d() : 0.0;
        double freqHz  = freqMHz * 1000000.0;

        std::cout << "[Hardware] Tuning RSPdx to: " << freqMHz << " MHz" << std::endl;

        if (sdr->TuneFrequency(freqHz)) {
            crow::response res(200);
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        } else {
            crow::response res(500, "Hardware Tuning Failed");
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        }
    });

    // =======================================================
    // ROUTE: DATABASE LOG MANAGEMENT
    // =======================================================
    CROW_ROUTE(app, "/api/logs")([]() {
        std::vector<RadioLog> logs = getAllLogs();
        crow::json::wvalue response;
        if (logs.empty()) {
            response = crow::json::wvalue(crow::json::type::List);
        } else {
            for (size_t i = 0; i < logs.size(); i++) {
                response[i]["id"]       = i;
                response[i]["freq"]     = std::to_string(logs[i].freq);
                response[i]["time"]     = logs[i].time;
                response[i]["location"] = logs[i].location;
                response[i]["name"]     = logs[i].channelName;
                response[i]["summary"]  = logs[i].summary;
                response[i]["rawT"]     = logs[i].rawT;
            }
        }
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/api/logs/delete").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Bad JSON");
        removeLog(x["freq"].d(), x["time"].i(), std::string(x["location"].s()));
        crow::response res(200);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/api/logs/save").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Bad JSON");
        insertLog(x["freq"].d(), x["time"].i(), std::string(x["location"].s()),
                  std::string(x["rawT"].s()), std::string(x["summary"].s()),
                  std::string(x["channelName"].s()));
        crow::response res(200);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // OPENAI ROUTES (Cloud-based)
    // =======================================================
    CROW_ROUTE(app, "/api/transcribe/openai").methods(crow::HTTPMethod::Post)
    ([apiKey](const crow::request& req) {
        std::string text = transcribeAudio("server/src/AudioFile/audio.wav", apiKey);
        crow::json::wvalue response;
        response["transcription"] = text;
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/api/summarize/openai").methods(crow::HTTPMethod::Post)
    ([apiKey](const crow::request& req) {
        auto x = crow::json::load(req.body);
        std::string summary = summarizeText(std::string(x["text"].s()), apiKey);
        crow::json::wvalue response;
        response["summary"] = summary;
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // LOCAL ROUTES (Whisper.cpp + Ollama)
    // =======================================================
    CROW_ROUTE(app, "/api/transcribe/local").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        std::string model_path = "server/src/whispertinytest/ggml-base.en.bin";
        std::string wav_path   = "server/src/AudioFile/audio.wav";

        WhisperTest transcriber(model_path);
        std::string text = transcriber.transcribe(wav_path);

        crow::json::wvalue response;
        response["transcription"] = text;
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/api/summarize/local").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body);
        std::string raw_text = x.has("text") ? std::string(x["text"].s()) : "";
        std::string summary = GenerateSummary(raw_text);

        crow::json::wvalue response;
        response["summary"] = summary;
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE: SERVE AUDIO FILES TO REACT
    // =======================================================
    CROW_ROUTE(app, "/whispertinytest/<string>")
    ([](std::string filename) {
        std::cout << "[Audio] React requested: " << filename << std::endl;

        std::string file_path = "server/src/whispertinytest/" + filename;
        std::ifstream file(file_path, std::ios::binary);

        if (!file) {
            std::cout << "[Audio] File not found: " << file_path << std::endl;
            crow::response res(404);
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        }

        std::ostringstream contents;
        contents << file.rdbuf();
        file.close();

        crow::response res(contents.str());
        res.set_header("Content-Type", "audio/wav");
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // SERVER STARTUP
    // =======================================================
    try {
        std::cout << "AetherGuard Backend active on port 8080." << std::endl;
        app.port(8080).multithreaded().run();
    } catch (const std::exception& e) {
        std::cerr << "Server Error: " << e.what() << std::endl;
    }

    curl_global_cleanup();
}

int main() {
    SdrHandler sdr;

    std::cout << "Starting AetherGuard SDR System..." << std::endl;

    if (!sdr.InitializeAPI()) {
        std::cerr << "Hardware initialization failed." << std::endl;
        return 1;
    }

    if (!sdr.StartStream(154500000.0)) {
        std::cerr << "Stream start failed." << std::endl;
        sdr.ShutdownSDR();
        return 1;
    }

    openFrontEnd(&sdr);

    sdr.ShutdownSDR();
    return 0;
}