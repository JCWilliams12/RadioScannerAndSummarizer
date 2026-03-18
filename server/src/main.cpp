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
// =======================================================
void setBandwidth(SdrHandler* sdr, sdrplay_api_Bw_MHzT bw, sdrplay_api_If_kHzT ifMode, bool isMock) {
    if (isMock) return; // Bypass hardware call in Mock Mode

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

// Passed the isMock flag into the frontend setup
void openFrontEnd(SdrHandler* sdr, bool isMock) {

    crow::SimpleApp app;
    createTable();
    
    curl_global_init(CURL_GLOBAL_ALL);
    auto env = loadEnv(".env");
    std::string apiKey = "";
    if (env.find("OPENAI_API_KEY") != env.end()) {
        apiKey = env["OPENAI_API_KEY"];
    } else {
        std::cerr << "CRITICAL ERROR: OPENAI_API_KEY not found in .env file!" << std::endl;
    }

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
    ([&sdr, isMock](const crow::request& req) { // Captured isMock
        auto json_data = crow::json::load(req.body);
        if (!json_data) {
            std::cerr << "[Record] Failed to parse JSON body!" << std::endl;
            crow::response err_res(400, "Invalid JSON");
            err_res.add_header("Access-Control-Allow-Origin", "*");
            return err_res;
        }

        double targetFreq = json_data["freq"].d();
        std::cout << "[Hardware] Async Record Request for " << targetFreq << " MHz" << std::endl;

        std::thread([sdr, targetFreq, isMock]() {
            if (isMock) {
                std::cout << "[Mock] Simulating 30-second recording sequence..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(30));
                std::cout << "[Mock] Recording complete. Ready for AI." << std::endl;
                return;
            }

            setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero, isMock);
            sdr->TuneFrequency(targetFreq * 1000000.0);
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

    // =======================================================
    // ROUTE: WIDEBAND SPECTRUM SCANNER (88.0 - 108.0 MHz)
    // =======================================================
    CROW_ROUTE(app, "/api/scan/wideband")
    ([&sdr, isMock]() { // Captured isMock
        if (isMock) {
            std::cout << "[Mock] Returning dummy station data for Wideband Sweep." << std::endl;
            std::vector<crow::json::wvalue> mock_stations = {
                {{"freq", 89.9}, {"name", "Mock FM 89.9"}},
                {{"freq", 101.1}, {"name", "Mock FM 101.1"}},
                {{"freq", 107.5}, {"name", "Mock FM 107.5"}}
            };
            crow::json::wvalue res;
            res["stations"] = std::move(mock_stations);
            crow::response response(res);
            response.add_header("Access-Control-Allow-Origin", "*");
            return response;
        }

        std::vector<crow::json::wvalue> found_stations;

        std::cout << "[Scanner] Switching to narrow bandwidth (200 kHz / IF_0_450)..." << std::endl;
        setBandwidth(sdr, sdrplay_api_BW_0_200, sdrplay_api_IF_0_450, isMock);

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

        sdr->TuneFrequency(87.9 * 1000000.0);
        sdr->ClearPowerHistory();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        float noiseFloor = sdr->GetCurrentPower();
        float squelchThreshold = noiseFloor + 12.0f;

        double bestFreq     = 0.0;
        float  bestRssi     = -999.0f;
        bool   inCluster    = false;
        int    clusterSteps = 0;

        for (double freqMHz = 88.1; freqMHz <= 107.9; freqMHz += 0.1) {
            sdr->TuneFrequency(freqMHz * 1000000.0);
            sdr->ClearPowerHistory();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            sdr->ClearPowerHistory();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));

            float currentRssi = sdr->GetCurrentPower();
            
            if (currentRssi >= squelchThreshold) {
                inCluster = true;
                clusterSteps++;
                if (currentRssi > bestRssi) {
                    bestRssi = currentRssi;
                    bestFreq = freqMHz;
                }
            } else {
                if (inCluster) {
                    if (clusterSteps >= 2) {
                        crow::json::wvalue station;
                        station["freq"] = bestFreq;
                        station["name"] = "FM Station " + std::to_string(bestFreq).substr(0, 5);
                        found_stations.push_back(std::move(station));
                    }
                    inCluster    = false;
                    bestRssi     = -999.0f;
                    clusterSteps = 0;
                }
            }
        }

        if (inCluster && clusterSteps >= 2) {
            crow::json::wvalue station;
            station["freq"] = bestFreq;
            station["name"] = "FM Station " + std::to_string(bestFreq).substr(0, 5);
            found_stations.push_back(std::move(station));
        }

        std::cout << "[Scanner] Restoring wideband mode (1.536 MHz / IF_Zero)..." << std::endl;
        setBandwidth(sdr, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero, isMock);

        sdrplay_api_DeviceParamsT* restoreParams = nullptr;
        sdrplay_api_GetDeviceParams(sdr->getDeviceHandle()->dev, &restoreParams);
        if (restoreParams && restoreParams->rxChannelA) {
            restoreParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
            sdrplay_api_Update(sdr->getDeviceHandle()->dev, sdr->getDeviceHandle()->tuner,
                sdrplay_api_Update_Ctrl_Agc,
                sdrplay_api_Update_Ext1_None);
        }

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
    ([&sdr, isMock](const crow::request& req) { // Captured isMock
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Bad JSON");

        double freqMHz = x.has("freq") ? x["freq"].d() : 0.0;
        double freqHz  = freqMHz * 1000000.0;

        std::cout << "[Hardware] Tuning to: " << freqMHz << " MHz" << std::endl;

        if (isMock || sdr->TuneFrequency(freqHz)) { // Bypass hardware tuning if mocked
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
    // ROUTE: DATABASE LOG MANAGEMENT ts just adds the test data for db 
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
    //Mock data is inserted here 
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
    CROW_ROUTE(app, "/api/audio/<string>")
    ([](std::string filename) {
        std::cout << "[Audio] React requested: " << filename << std::endl;

        std::string file_path = "server/src/AudioFile/" + filename;
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
    bool isMock = false;

    std::cout << "Starting AetherGuard SDR System..." << std::endl;

    if (!sdr.InitializeAPI()) {
        std::cerr << "[WARNING] Hardware initialization failed! Booting in MOCK MODE." << std::endl;
        isMock = true; // Engage Mock Mode instead of crashing
    } else if (!sdr.StartStream(154500000.0)) {
        std::cerr << "Stream start failed." << std::endl;
        sdr.ShutdownSDR();
        return 1;
    }

    openFrontEnd(&sdr, isMock);

    if (!isMock) {
        sdr.ShutdownSDR();
    }
    
    return 0;
}