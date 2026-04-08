#include <iostream>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <hiredis/hiredis.h>
#include <curl/curl.h>
#include <fstream>
#include <sstream>

#include "dbcorefunctions.hpp"
#include "dbcorefilter.hpp"
#include "crow.h"

extern "C" {
    #include "sqlite3.h"
}

std::mutex ws_audio_mtx;
std::unordered_set<crow::websocket::connection*> audio_clients;
std::mutex ws_status_mtx;
std::unordered_set<crow::websocket::connection*> status_clients;

redisContext *g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

// Seeding the database for mockup versions 
void seedDatabase() {
    if (!getAllLogs().empty()) {
        std::cout << "[API] Database already has data. Skipping seed." << std::endl;
        return;
    }

    sqlite3 *db;
    if (sqlite3_open(DB_NAME, &db) == SQLITE_OK) {
        const char* sql = 
            "INSERT INTO RadioLogs (freq, time, location, rawT, summary, channelName, audioFilePath) VALUES "
            "(154.280, strftime('%s', 'now'), 'North Precinct', 'Officer needs assistance.', 'Officer request', 'Bham Police 1', '/api/audio/audio.wav'),"
            "(462.562, strftime('%s', 'now', '-5 minutes'), 'Greystone', 'Order is ready at window two.', 'Drive-thru comms', 'Fast Food Ops', '/api/audio/audio.wav'),"
            "(160.230, strftime('%s', 'now', '-1 hour'), 'Rail Yard', 'Train 42 is cleared on track 3.', 'Train clearance', 'Railroad Ch 1', '/api/audio/audio.wav');";
        
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, 0, 0, &errMsg) != SQLITE_OK) {
            std::cerr << "[API] Seed error: " << errMsg << std::endl;
            sqlite3_free(errMsg);
        } else {
            std::cout << "[API] Successfully injected test cases into the database!" << std::endl;
        }
        sqlite3_close(db);
    } else {
        std::cerr << "[API] Failed to open DB for seeding." << std::endl;
    }
}

void redisListenerThread() {
    redisContext *c = redisConnect("ag-redis", 6379);
    if (!c || c->err) return;
    
    redisReply *reply = (redisReply*)redisCommand(c, "SUBSCRIBE live_audio ws_updates");
    if (reply) freeReplyObject(reply);

    while (redisGetReply(c, (void**)&reply) == REDIS_OK) {
        if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            std::string type = reply->element[0]->str ? reply->element[0]->str : "";
            
            if (type == "message") {
                std::string channel = reply->element[1]->str ? reply->element[1]->str : "";
                
                if (channel == "live_audio") {
                    std::string payload(reply->element[2]->str, reply->element[2]->len);
                    std::lock_guard<std::mutex> lock(ws_audio_mtx);
                    for (auto client : audio_clients) {
                        if (client) client->send_binary(payload);
                    }
                } 
                else if (channel == "ws_updates") {
                    std::string payload(reply->element[2]->str, reply->element[2]->len);
                    std::lock_guard<std::mutex> lock(ws_status_mtx);
                    for (auto client : status_clients) {
                        if (client) client->send_text(payload);
                    }
                }
            }
        }
        if (reply) freeReplyObject(reply);
    }
    redisFree(c);
}

crow::response makeCorsResponse(crow::json::wvalue body, int code = 200) {
    crow::response res(code, body);
    res.add_header("Access-Control-Allow-Origin", "*");
    return res;
}

// 1. Add the missing getEnvVar function near the top of your file (above callOpenAI)
std::string getEnvVar(std::string key) {
    char * val = getenv(key.c_str());
    return val == NULL ? std::string("") : std::string(val);
}

// 2. Helper for cURL
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

// 3. Helper function to send the payload to OpenAI
std::string callOpenAI(const crow::json::wvalue& payload, const std::string& apiKey) {
    CURL* curl = curl_easy_init();
    std::string responseString;
    if (curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());

        std::string payloadStr = payload.dump();

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return responseString;
}

int main() {
    crow::SimpleApp app;
    curl_global_init(CURL_GLOBAL_ALL);

    if (!createTable()) {
        std::cerr << "[API] Database error." << std::endl;
        return 1;
    }

    seedDatabase();

    std::cout << "[API] Connecting to Redis..." << std::endl;
    for(int i=0; i<5; i++) {
        g_redis_pub = redisConnect("ag-redis", 6379);
        if (g_redis_pub && !g_redis_pub->err) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!g_redis_pub || g_redis_pub->err) return 1;

    std::thread(redisListenerThread).detach();


    // Manages websocket connect for streaming real-time audio data
    CROW_WEBSOCKET_ROUTE(app, "/ws/audio")
    .onopen([](crow::websocket::connection& conn) { std::lock_guard<std::mutex> lock(ws_audio_mtx); audio_clients.insert(&conn); })
    .onclose([](crow::websocket::connection& conn, const std::string&) { std::lock_guard<std::mutex> lock(ws_audio_mtx); audio_clients.erase(&conn); });

    // Manages websocket conection to broadcast system updates (scan progress and AI processing scan) in the front end 
    CROW_WEBSOCKET_ROUTE(app, "/ws/status")
    .onopen([](crow::websocket::connection& conn) { std::lock_guard<std::mutex> lock(ws_status_mtx); status_clients.insert(&conn); })
    .onclose([](crow::websocket::connection& conn, const std::string&) { std::lock_guard<std::mutex> lock(ws_status_mtx); status_clients.erase(&conn); });

    // Focuses on instructing the SDR hardware to shift to a specific grequenciy via Redis 
    CROW_ROUTE(app, "/api/scan/tune").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        crow::json::wvalue cmd; cmd["command"] = "TUNE"; cmd["freq"] = x["freq"].d();
        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "tuned"}});
    });

    // Publishes Live_listen and stop_live commands via redis to toggel real-time audio stream from SDR
    CROW_ROUTE(app, "/api/scan/live").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        std::string action = x["action"].s();

        crow::json::wvalue cmd;
        cmd["command"] = (action == "start") ? "LIVE_LISTEN" : "STOP_LIVE";

        if (action == "start" && x.has("freq")) {
            cmd["freq"] = x["freq"].d();
        }

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", action == "start" ? "live_started" : "live_stopped"}});
    });

// Record command via Redis, triggering sdr hardware to capture 30-second audio sample of selected frequency 
    CROW_ROUTE(app, "/api/scan/record").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        
        crow::json::wvalue cmd; 
        cmd["command"] = "RECORD"; 
        cmd["freq"] = x["freq"].d();
        
        // --- THE MISSING LINK ---
        // Forward the exact timestamp to the SDR worker!
        if (x.has("timestamp")) {
            cmd["timestamp"] = x["timestamp"].i();
        }

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "recording_started"}});
    });

    // Triggers a full spectrum sweep by publishing a scan command to the sdr. 
    // Temporily subscribes to redis to wait for results and returns array of discovered stations 
    CROW_ROUTE(app, "/api/scan/wideband")
    ([]() {
        crow::json::wvalue cmd;
        cmd["command"] = "SCAN";
        {
            std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
            redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        }

        redisContext *sub = redisConnect("ag-redis", 6379);
        redisReply *reply = (redisReply*)redisCommand(sub, "SUBSCRIBE ws_updates");
        if (reply) freeReplyObject(reply);

        crow::json::wvalue final_stations;
        
        while (redisGetReply(sub, (void**)&reply) == REDIS_OK) {
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
                std::string msg_type = reply->element[0]->str;
                if (msg_type == "message") {
                    auto data = crow::json::load(reply->element[2]->str);
                    if (data && data["event"].s() == "scan_complete") {
                        final_stations = std::move(data["stations"]);
                        freeReplyObject(reply);
                        break; 
                    }
                }
            }
            freeReplyObject(reply);
        }
        redisFree(sub);

        crow::json::wvalue res;
        res["stations"] = std::move(final_stations);
        crow::response response(res);
        response.add_header("Access-Control-Allow-Origin", "*");
        return response;
    });

    // Fetches and returns all saved radio interception logs from SQLite database 
    CROW_ROUTE(app, "/api/logs")([]() {
        std::vector<RadioLog> logs = getAllLogs();
        crow::json::wvalue res;
        if (logs.empty()) res = crow::json::wvalue::list();
        else {
            for (size_t i = 0; i < logs.size(); i++) {
                res[i]["freq"] = logs[i].freq; 
                res[i]["time"] = logs[i].time;
                res[i]["location"] = logs[i].location; 
                res[i]["name"] = logs[i].channelName;
                res[i]["summary"] = logs[i].summary; 
                res[i]["rawT"] = logs[i].rawT;
                res[i]["audioFilePath"] = logs[i].audioFilePath; 
            }
        }
        return makeCorsResponse(res);
    });

    // Retrieves a specific subset of database logs filtered given by the channel from user 
    CROW_ROUTE(app, "/api/filter/channel")
    ([](const crow::request& req) {
        std::string channel = req.url_params.get("name") ? req.url_params.get("name") : "";
        if (channel.empty()) return makeCorsResponse(crow::json::wvalue::list());

        std::vector<RadioLog> logs = filterByChannelName(channel);
        crow::json::wvalue res;
        
        if (logs.empty()) {
            res = crow::json::wvalue::list();
        } else {
            for (size_t i = 0; i < logs.size(); i++) {
                res[i]["freq"]     = logs[i].freq;
                res[i]["time"]     = logs[i].time;
                res[i]["location"] = logs[i].location;
                res[i]["name"]     = logs[i].channelName;
                res[i]["summary"]  = logs[i].summary;
                res[i]["rawT"]     = logs[i].rawT;
            }
        }
        return makeCorsResponse(res); 
    });
// Multi-parameter search across the database, attempting to match the query against freq, location or channel name. 
// Prevents duplicate results 
CROW_ROUTE(app, "/api/search")
    ([](const crow::request& req) {
        std::string q = req.url_params.get("q") ? req.url_params.get("q") : "";
        std::vector<RadioLog> logs;

        if (q.empty()) {
            logs = getAllLogs();
        } else {
            try { 
                logs = filterByFrequency(std::stod(q)); 
            } catch (...) { 
                auto locLogs = filterByLocation(q);
                auto chanLogs = filterByChannelName(q);
                
                logs = std::move(locLogs);
                
                for (const auto& chanLog : chanLogs) {
                    bool isDuplicate = false;
                    for (const auto& existingLog : logs) {
                        if (existingLog.time == chanLog.time && existingLog.freq == chanLog.freq) {
                            isDuplicate = true;
                            break;
                        }
                    }
                    if (!isDuplicate) {
                        logs.push_back(chanLog);
                    }
                }
            }
        }
        
        crow::json::wvalue res;
        if (logs.empty()) res = crow::json::wvalue::list();
        else {
            for (size_t i = 0; i < logs.size(); i++) {
                res[i]["freq"] = logs[i].freq; res[i]["time"] = logs[i].time;
                res[i]["location"] = logs[i].location; res[i]["text"] = logs[i].rawT;
                res[i]["summary"] = logs[i].summary; res[i]["name"] = logs[i].channelName;
            }
        }
        return makeCorsResponse(res); 
    });

    CROW_ROUTE(app, "/api/agent/chat").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"error", "Invalid JSON"}});

        std::string apiKey = getEnvVar("OPENAI_API_KEY"); 
        if (apiKey.empty()) return makeCorsResponse({{"error", "OpenAI API key missing"}});

        // --- 1. Construct Initial Payload ---
        crow::json::wvalue payload;
        payload["model"] = "gpt-4o-mini";
        
        // Build Tools Safely (Now using 'searchDatabase' mapped to advancedSearch)
        crow::json::wvalue::list tools;
        crow::json::wvalue tool;
        tool["type"] = "function";
        
        crow::json::wvalue funcObj;
        funcObj["name"] = "searchDatabase";
        funcObj["description"] = "Search the intercepted radio database by frequency or keyword. Use this to find any requested logs.";
        
        crow::json::wvalue paramsObj;
        paramsObj["type"] = "object";
        
        crow::json::wvalue propsObj;
        
        crow::json::wvalue freqObj; 
        freqObj["type"] = "string"; 
        freqObj["description"] = "Frequency in MHz (e.g. '154.280')";
        propsObj["freq"] = std::move(freqObj);
        
        crow::json::wvalue keywordObj; 
        keywordObj["type"] = "string"; 
        keywordObj["description"] = "Keyword to search in summaries or raw text";
        propsObj["keyword"] = std::move(keywordObj);
        
        paramsObj["properties"] = std::move(propsObj);
        funcObj["parameters"] = std::move(paramsObj);
        tool["function"] = std::move(funcObj);
        tools.push_back(std::move(tool));
        
        payload["tools"] = std::move(tools);
        payload["tool_choice"] = "auto";

        // Build Messages Safely
        crow::json::wvalue::list messages;
        crow::json::wvalue sysMsg;
        sysMsg["role"] = "system";
        sysMsg["content"] = "You are the AetherGuard Database Agent, a strict, highly analytical radio intelligence assistant. Your SOLE purpose is to query the SQLite database to answer questions about intercepted radio transmissions. \n\nCRITICAL RULE: You are completely forbidden from answering general knowledge questions, giving lifestyle advice, or discussing topics outside the AetherGuard database. If a user asks an unrelated question (e.g., restaurants, weather, general trivia) or a question you cannot answer using your database search tools, do not use your general knowledge. You MUST reply exactly with: 'This query is outside my operational scope. I can only provide intelligence based on intercepted radio logs.'";        messages.push_back(std::move(sysMsg));

        for (const auto& msg : body["messages"]) {
            crow::json::wvalue m;
            m["role"] = msg["role"].s();
            m["content"] = msg["content"].s();
            messages.push_back(std::move(m));
        }

        payload["messages"] = std::move(messages);

        // --- 2. Execute First Call ---
        std::string firstResponse = callOpenAI(payload, apiKey);
        auto firstResJson = crow::json::load(firstResponse);

        if (!firstResJson || !firstResJson.has("choices")) {
            return makeCorsResponse({{"answer", "Error: OpenAI API failure."}});
        }

        auto messageNode = firstResJson["choices"][0]["message"];

        // --- 3. Handle Tool Calls ---
        if (messageNode.has("tool_calls")) {
            crow::json::wvalue payload2;
            payload2["model"] = "gpt-4o-mini";
            
            crow::json::wvalue::list messages2;
            
            crow::json::wvalue sysMsg2;
            sysMsg2["role"] = "system";
            sysMsg2["content"] = "You are the AetherGuard Database Agent, a strict, highly analytical radio intelligence assistant. Your SOLE purpose is to query the SQLite database to answer questions about intercepted radio transmissions. \n\nCRITICAL RULE: You are completely forbidden from answering general knowledge questions, giving lifestyle advice, or discussing topics outside the AetherGuard database. If a user asks an unrelated question (e.g., restaurants, weather, general trivia) or a question you cannot answer using your database search tools, do not use your general knowledge. You MUST reply exactly with: 'This query is outside my operational scope. I can only provide intelligence based on intercepted radio logs.'";            messages2.push_back(std::move(sysMsg2));

            for (const auto& msg : body["messages"]) {
                crow::json::wvalue m;
                m["role"] = msg["role"].s();
                m["content"] = msg["content"].s();
                messages2.push_back(std::move(m));
            }

            crow::json::wvalue assistantMsg;
            assistantMsg["role"] = "assistant";
            crow::json::wvalue::list tCalls;
            
            for (const auto& tc : messageNode["tool_calls"]) {
                crow::json::wvalue tCall;
                tCall["id"] = tc["id"].s();
                tCall["type"] = tc["type"].s();
                tCall["function"]["name"] = tc["function"]["name"].s();
                tCall["function"]["arguments"] = tc["function"]["arguments"].s();
                tCalls.push_back(std::move(tCall));
            }
            assistantMsg["tool_calls"] = std::move(tCalls);
            messages2.push_back(std::move(assistantMsg));

            for (const auto& tc : messageNode["tool_calls"]) {
                std::string toolId = tc["id"].s();
                std::string funcName = tc["function"]["name"].s();
                std::string argsStr = tc["function"]["arguments"].s();
                auto args = crow::json::load(argsStr);

                std::string dbResultText = "No results found.";

                // Upgraded to use your powerful advancedSearch function
                if (funcName == "searchDatabase" || funcName == "searchDatabaseKeywords") {
                    std::string freq = "";
                    if (args.has("freq")) freq = std::string(args["freq"].s());
                    
                    std::string keyword = "";
                    if (args.has("keyword")) keyword = std::string(args["keyword"].s());
                    
                    std::vector<RadioLog> logs = advancedSearch(freq, "", keyword, "", "");
                    
                    if (!logs.empty()) {
                        dbResultText = "Found " + std::to_string(logs.size()) + " logs:\n";
                        int count = 0;
                        for (const auto& log : logs) {
                            if (count++ >= 10) break; // Token limit protection
                            dbResultText += "- [" + std::to_string(log.time) + "] " + log.channelName + " (" + std::to_string(log.freq) + " MHz): " + log.summary + "\n";
                        }
                    }
                }

                crow::json::wvalue toolResultMsg;
                toolResultMsg["role"] = "tool";
                toolResultMsg["tool_call_id"] = toolId;
                toolResultMsg["content"] = dbResultText;
                messages2.push_back(std::move(toolResultMsg));
            }

            payload2["messages"] = std::move(messages2);

            // --- 4. Final Call to OpenAI ---
            std::string finalResponse = callOpenAI(payload2, apiKey);
            auto finalResJson = crow::json::load(finalResponse);
            
            if (!finalResJson || !finalResJson.has("choices")) {
                return makeCorsResponse({{"answer", "Error: OpenAI second API failure."}});
            }
            
            std::string finalAnswer = finalResJson["choices"][0]["message"]["content"].s();
            return makeCorsResponse({{"answer", finalAnswer}});
        }

        // If no tools were used, return standard text
        std::string directAnswer = messageNode["content"].s();
        return makeCorsResponse({{"answer", directAnswer}});
    });

    // Dev utility route to populate database with dummy data for testing 
    CROW_ROUTE(app, "/api/dev/seed").methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
    ([]() {
        seedDatabase();
        return makeCorsResponse({{"status", "database_seeded_successfully"}});
    });

    // Removes specific log entry from DB that user has selected 
    CROW_ROUTE(app, "/api/logs/delete").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}, {"message", "Invalid JSON"}}, 400);

        double freq = body["freq"].d();
        long long time = body["time"].i();
        std::string location = body["location"].s();

        if (removeLog(freq, time, location)) {
            return makeCorsResponse({{"status", "success"}});
        } else {
            return makeCorsResponse({{"status", "error"}, {"message", "Log not found"}}, 404);
        }
    });

// Inserts new completed records (freq, time, raw text and AI sum) into DB 
    CROW_ROUTE(app, "/api/logs/save").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}, {"message", "Invalid JSON"}}, 400);

        double freq = body["freq"].d();
        long long time = body["time"].i(); 
        std::string location = body["location"].s();
        std::string rawT = body["rawT"].s();
        std::string summary = body["summary"].s();
        std::string channelName = body["channelName"].s();
        
        // --- THE C++ TYPING FIX ---
        // Explicitly cast both sides to std::string so the compiler is happy
        // DANIEL FIX ME ! 
        std::string audioFilePath = body.has("audioFilePath") 
            ? std::string(body["audioFilePath"].s()) 
            : std::string("/api/audio/captured_" + std::to_string(time) + ".wav");

        insertLog(freq, time, location, rawT, summary, channelName, audioFilePath);  
              
        return makeCorsResponse({{"status", "success"}});
    });

    CROW_ROUTE(app, "/api/search/advanced")
    ([](const crow::request& req) {
        std::string freq = req.url_params.get("freq") ? req.url_params.get("freq") : "";
        std::string loc = req.url_params.get("loc") ? req.url_params.get("loc") : "";
        std::string keyword = req.url_params.get("keyword") ? req.url_params.get("keyword") : "";
        std::string startStr = req.url_params.get("start") ? req.url_params.get("start") : "";
        std::string endStr = req.url_params.get("end") ? req.url_params.get("end") : "";

        std::vector<RadioLog> logs = advancedSearch(freq, loc, keyword, startStr, endStr);
        
        crow::json::wvalue res;
        if (logs.empty()) res = crow::json::wvalue::list();
        else {
            for (size_t i = 0; i < logs.size(); i++) {
                res[i]["freq"] = logs[i].freq; 
                res[i]["time"] = logs[i].time;
                res[i]["location"] = logs[i].location; 
                res[i]["rawT"] = logs[i].rawT;
                res[i]["summary"] = logs[i].summary; 
                res[i]["name"] = logs[i].channelName;
                res[i]["audioFilePath"] = logs[i].audioFilePath;
            }
        }
        return makeCorsResponse(res); 
    });

    // Dev utility rout that completely wipes all records from RadioLogs DB table 
    CROW_ROUTE(app, "/api/dev/clear").methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
    ([]() {
        sqlite3 *db;
        if (sqlite3_open(DB_NAME, &db) == SQLITE_OK) {
            sqlite3_exec(db, "DROP TABLE IF EXISTS RadioLogs;", 0, 0, 0);
            sqlite3_close(db);
            
            createTable(); 
        }
        return makeCorsResponse({{"status", "database_wiped_and_recreated"}});
    });

    // Publishes command to AI owrker to transcribe an audio file using the whisper.cpp
    CROW_ROUTE(app, "/api/transcribe/local").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}}, 400);

        crow::json::wvalue cmd;
        cmd["command"] = "TRANSCRIBE_LOCAL";
        cmd["freq"] = body["freq"].d();

        // Adding file path 
        if (body.has("file")) {
            cmd["file"] = body["file"].s();
        }

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH ai_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "transcribing"}});
    });
    // Publishes command to AI worker to transcribe a text summary of trancsription using local model LLama.cpp 
    CROW_ROUTE(app, "/api/summarize/local").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}}, 400);

        crow::json::wvalue cmd;
        cmd["command"] = "SUMMARIZE_LOCAL";
        cmd["freq"] = body["freq"].d();
        cmd["text"] = body["text"].s();

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH ai_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "summarizing"}});
    });
    // Publishes command to Ai worker to transcribe an audio file using 3o mini cloud model 
    CROW_ROUTE(app, "/api/transcribe/openai").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}}, 400);

        crow::json::wvalue cmd;
        cmd["command"] = "TRANSCRIBE_OPENAI";
        cmd["freq"] = body["freq"].d();

        // adding file path 
        if (body.has("file")) {
            cmd["file"] = body["file"].s();
        }

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH ai_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "transcribing"}});
    });
    // Publishes cmmand to AI worker to generate a text from transcribed text using open ais cloud model 
    CROW_ROUTE(app, "/api/summarize/openai").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}}, 400);

        crow::json::wvalue cmd;
        cmd["command"] = "SUMMARIZE_OPENAI";
        cmd["freq"] = body["freq"].d();
        cmd["text"] = body["text"].s();

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH ai_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "summarizing"}});
    });

    CROW_ROUTE(app, "/stations")([]() { return makeCorsResponse(crow::json::wvalue::list()); });

    // Serves the audio files to the frontend for playback
    CROW_ROUTE(app, "/api/audio/<string>")
    ([](const crow::request& req, std::string filename){
        
        std::string filepath = "/app/shared/audio/" + filename; 
        
        // Open file in binary mode
        std::ifstream file(filepath, std::ios::binary);
        if (!file.good()) {
            std::cout << "\n[AUDIO ERROR] File missing at: " << filepath << std::endl;
            return crow::response(404, "File not found");
        }
        
        std::cout << "\n[AUDIO SUCCESS] Bypassing sandbox. Loading into memory: " << filepath << std::endl;
        
        // Read the exact binary data into a buffer
        std::ostringstream buffer;
        buffer << file.rdbuf();
        
        // Return the binary data directly in the response body!
        crow::response res(buffer.str());
        res.add_header("Content-Type", "audio/wav");
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Accept-Ranges", "bytes"); // Helps the browser audio player
        return res;
    });
    // --- END OF NEW ROUTE ---

    std::cout << "[API] Gateway online at http://0.0.0.0:8080" << std::endl;
    app.port(8080).multithreaded().run();
    return 0;
}
