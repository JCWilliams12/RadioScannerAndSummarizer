#include <iostream>
#include <string>
#include <stdio.h>
#include <vector>
#include "dbcorefunctions.hpp"
#include "dbcorefilter.hpp"
#include "crow.h"
#include "ollamatest.hpp"
#include "whispertinytest.hpp"

//for audio playback 
#include <fstream>
#include <sstream>

void openFrontEnd(){
    crow::SimpleApp app;

    // =======================================================
    // ROUTE 1: GET LIVE STATIONS (For the Scanning View)
    // =======================================================
    CROW_ROUTE(app, "/stations")([](){
        // NOTE: Replace this mock data with your actual live scanner logic later!
        crow::json::wvalue station1 = {{"id", 1}, {"name", "Live Scanner 1"}, {"freq", "144.200"}};
        crow::json::wvalue station2 = {{"id", 2}, {"name", "Live Scanner 2"}, {"freq", "155.100"}};
        
        std::vector<crow::json::wvalue> station_list = {station1, station2};
        
        crow::json::wvalue json_data(station_list);
        crow::response res(json_data); 
        
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE 1.5: GET SAVED LOGS (For the Database View)
    // =======================================================
    CROW_ROUTE(app, "/api/logs")([](){
        std::vector<RadioLog> logs = getAllLogs();
        crow::json::wvalue response;
        
        if (logs.empty()) {
            response = crow::json::wvalue(crow::json::type::List);
        } else {
            for (size_t i = 0; i < logs.size(); i++) {
                response[i]["id"] = i; 
                response[i]["freq"] = std::to_string(logs[i].freq); // Updated to .freq
                response[i]["time"] = logs[i].time;
                response[i]["location"] = logs[i].location;
                // Map the C++ 'channelName' to React's 'name'
                response[i]["name"] = logs[i].channelName; 
                // Pass the summary and raw text to React!
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
        
        std::cout << "\n--- INCOMING DELETE REQUEST ---" << std::endl;
        
        auto x = crow::json::load(req.body);
        if (!x) {
            crow::response res(400, "Bad JSON");
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        }

        // Extract variables, INCLUDING LOCATION NOW!
        double freq = x.has("freq") ? x["freq"].d() : 0.0;
        long long time = x.has("time") ? x["time"].i() : 0;
        std::string location = x.has("location") ? std::string(x["location"].s()) : "";

        std::cout << "Executing removeLog(" << freq << ", " << time << ", " << location << ")..." << std::endl;
        
        // Pass all 3 keys to removeLog
        removeLog(freq, time, location);

        crow::response res(200);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE 2.5: SAVE A LOG (Brand New!)
    // =======================================================
    CROW_ROUTE(app, "/api/logs/save").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        
        std::cout << "\n--- INCOMING SAVE REQUEST ---" << std::endl;
        
        auto x = crow::json::load(req.body);
        if (!x) {
            crow::response res(400, "Bad JSON");
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        }

        // Extract all 6 fields from React
        double freq = x.has("freq") ? x["freq"].d() : 0.0;
        long long time = x.has("time") ? x["time"].i() : 0;
        std::string location = x.has("location") ? std::string(x["location"].s()) : "Unknown";
        std::string rawT = x.has("rawT") ? std::string(x["rawT"].s()) : "No raw text provided.";
        std::string summary = x.has("summary") ? std::string(x["summary"].s()) : "No summary available.";
        std::string channelName = x.has("channelName") ? std::string(x["channelName"].s()) : "Unknown Station";
        
        insertLog(freq, time, location, rawT, summary, channelName);

        crow::response res(200);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE 3: CATCH-ALL 404 LOGGER
    // =======================================================
    app.catchall_route()([](const crow::request& req, crow::response& res) {
        std::cout << "\n[404 DEBUG] Frontend asked for URL: " << req.url 
                  << " | Method: " << crow::method_name(req.method) << std::endl;
        
        res.code = 404;
        res.add_header("Access-Control-Allow-Origin", "*");
        res.body = "Route completely missed";
        res.end();
    });

    // =======================================================
    // ROUTE: STEP 1 - WHISPER TRANSCRIPTION
    // =======================================================
    CROW_ROUTE(app, "/api/transcribe").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        std::cout << "\n--- STEP 1: TRANSCRIPTION REQUEST ---" << std::endl;

        std::string base_folder = "server/src/whispertinytest/";
        std::string model_path  = base_folder + "ggml-base.en.bin";
        std::string wav_path    = base_folder + "audio.wav";

        WhisperTest transcriber(model_path);
        
        std::string text = transcriber.transcribe(wav_path);
        std::cout << "Whisper complete!" << std::endl;

        crow::json::wvalue response;
        response["transcription"] = text;

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    // =======================================================
    // ROUTE: STEP 2 - OLLAMA SUMMARIZATION
    // =======================================================
    CROW_ROUTE(app, "/api/summarize").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        std::cout << "\n--- STEP 2: SUMMARIZE REQUEST ---" << std::endl;
        
        auto x = crow::json::load(req.body);
        if (!x) {
            crow::response res(400, "Bad JSON");
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        }

        // Grab the raw text that React sent us
        std::string raw_text = x.has("text") ? std::string(x["text"].s()) : "";
        
        // Pass it to your Ollama function!
        std::cout << "Asking Ollama for a summary..." << std::endl;
        std::string summary = GenerateSummary(raw_text); 
        std::cout << "Ollama complete!" << std::endl;

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
        
        std::cout << "\n[AUDIO DEBUG] React requested file: " << filename << std::endl;

        // Construct the file path
        std::string file_path = "server/src/whispertinytest/" + filename; 

        // Open the file
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            std::cout << "[AUDIO DEBUG] Failed to find: " << file_path << std::endl;
            crow::response res(404);
            res.add_header("Access-Control-Allow-Origin", "*");
            return res;
        }

        // Read the file contents
        std::ostringstream contents;
        contents << file.rdbuf();
        file.close();

        // Send the file to React
        crow::response res(contents.str());
        res.set_header("Content-Type", "audio/wav");
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    std::cout << "AetherGuard running on port 8080..." << std::endl;
    app.port(8080).multithreaded().run();

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