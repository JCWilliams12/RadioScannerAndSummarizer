#include <iostream>
#include <string>
#include <stdio.h>
#include <vector>
#include <curl/curl.h>
#include "dbcorefunctions.hpp"
#include "dbcorefilter.hpp"
#include "crow.h"
#include "ollamatest.hpp"
#include "whispertinytest.hpp"
#include "openai.hpp"

void openFrontEnd() {
    crow::SimpleApp app;

    // 1. Initialize cURL and Load API Key
    curl_global_init(CURL_GLOBAL_ALL);
    auto env = loadEnv(".env");
    std::string apiKey = "";
    if (env.find("OPENAI_API_KEY") != env.end()) {
        apiKey = env["OPENAI_API_KEY"];
    } else {
        std::cerr << "CRITICAL ERROR: OPENAI_API_KEY not found in .env file!" << std::endl;
    }

    // =======================================================
    // ROUTE 1: GET LIVE STATIONS
    // =======================================================
    CROW_ROUTE(app, "/stations")([]() {
        crow::json::wvalue station1 = {{"id", 1}, {"name", "Live Scanner 1"}, {"freq", "144.200"}};
        crow::json::wvalue station2 = {{"id", 2}, {"name", "Live Scanner 2"}, {"freq", "155.100"}};
        
        std::vector<crow::json::wvalue> station_list = {station1, station2};
        crow::json::wvalue json_data(station_list);
        crow::response res(json_data); 
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE 1.5: GET SAVED LOGS
    // =======================================================
    CROW_ROUTE(app, "/api/logs")([]() {
        std::vector<RadioLog> logs = getAllLogs();
        crow::json::wvalue response;
        
        if (logs.empty()) {
            response = crow::json::wvalue(crow::json::type::List);
        } else {
            for (size_t i = 0; i < logs.size(); i++) {
                response[i]["id"] = i; 
                response[i]["freq"] = std::to_string(logs[i].freq);
                response[i]["time"] = logs[i].time;
                response[i]["location"] = logs[i].location;
                response[i]["name"] = logs[i].channelName; 
                response[i]["summary"] = logs[i].summary;
                response[i]["rawT"] = logs[i].rawT;
            }
        }
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res; 
    });

    // =======================================================
    // ROUTE 2: DELETE A LOG 
    // =======================================================
    CROW_ROUTE(app, "/api/logs/delete").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Bad JSON");

        double freq = x.has("freq") ? x["freq"].d() : 0.0;
        long long time = x.has("time") ? x["time"].i() : 0;
        std::string location = x.has("location") ? std::string(x["location"].s()) : "";

        removeLog(freq, time, location);

        crow::response res(200);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE 2.5: SAVE A LOG
    // =======================================================
    CROW_ROUTE(app, "/api/logs/save").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body);
        if (!x) return crow::response(400, "Bad JSON");

        double freq = x.has("freq") ? x["freq"].d() : 0.0;
        long long time = x.has("time") ? x["time"].i() : 0;
        std::string location = x.has("location") ? std::string(x["location"].s()) : "Unknown";
        std::string rawT = x.has("rawT") ? std::string(x["rawT"].s()) : "No raw text.";
        std::string summary = x.has("summary") ? std::string(x["summary"].s()) : "No summary.";
        std::string channelName = x.has("channelName") ? std::string(x["channelName"].s()) : "Unknown Station";
        
        insertLog(freq, time, location, rawT, summary, channelName);

        crow::response res(200);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // OPENAI ROUTES (Cloud-based)
    // =======================================================
    CROW_ROUTE(app, "/api/transcribe/openai").methods(crow::HTTPMethod::Post)
    ([apiKey](const crow::request& req) {
        std::string wav_path = "server/src/whispertinytest/audio.wav";
        std::string text = transcribeAudio(wav_path, apiKey);
        
        crow::json::wvalue response;
        response["transcription"] = text;
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/api/summarize/openai").methods(crow::HTTPMethod::Post)
    ([apiKey](const crow::request& req) {
        auto x = crow::json::load(req.body);
        std::string raw_text = x.has("text") ? std::string(x["text"].s()) : "";
        std::string summary = summarizeText(raw_text, apiKey); 

        crow::json::wvalue response;
        response["summary"] = summary;
        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // LOCAL ROUTES (Ollama / Whisper.cpp)
    // =======================================================
    CROW_ROUTE(app, "/api/transcribe/local").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        std::string model_path = "server/src/whispertinytest/ggml-base.en.bin";
        std::string wav_path   = "server/src/whispertinytest/audio.wav";
        
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
    // SERVER STARTUP & ERROR HANDLING
    // =======================================================
    try {
        std::cout << "AetherGuard attempting to bind to port 8080..." << std::endl;
        app.port(8080).multithreaded().run();
    } 
    catch (const std::exception& e) {
        std::cerr << "\n[CRITICAL SERVER ERROR]: " << e.what() << std::endl;
    } 

    curl_global_cleanup();
}



int main() {
    // Initialize DB table before starting server
    createTable();

    // UPDATED SIGNATURE: freq, time, location, rawT, summary, channelName
    insertLog(144.200, 1718900000, "Birmingham, AL", "[Raw Audio Data]", "Testing signal strength", "Test Station");
    insertLog(182.500, 1289621000, "Decatur, AL", "[Raw Audio Data]", "AI summary", "Wocka Flocka");
    insertLog(678.6767, 7518900000, "Mobile, AL", "[Raw Audio Data]", "AI summary", "Skibidi Toilet");
    insertLog(157.570, 9718900000, "Huntsville, AL", "[Raw Audio Data]", "AI summary", "Big Leagues");
    // Launch the Crow server
    openFrontEnd();
    
    return 0;
}