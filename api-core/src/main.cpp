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

// 1. Include SQLite for the seed function
extern "C" {
    #include "sqlite3.h"
}

std::mutex ws_audio_mtx;
std::unordered_set<crow::websocket::connection*> audio_clients;
std::mutex ws_status_mtx;
std::unordered_set<crow::websocket::connection*> status_clients;

redisContext *g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

// ==========================================
// DB SEED FUNCTION
// ==========================================
void seedDatabase() {
    // SMART SEED: Only inject if the database is completely empty!
    if (!getAllLogs().empty()) {
        std::cout << "[API] Database already has data. Skipping seed." << std::endl;
        return;
    }

    sqlite3 *db;
    if (sqlite3_open(DB_NAME, &db) == SQLITE_OK) {
        const char* sql = 
            "INSERT INTO RadioLogs (freq, time, location, rawT, summary, channelName) VALUES "
            "(154.280, strftime('%s', 'now'), 'North Precinct', 'Officer needs assistance.', 'Officer request', 'Bham Police 1'),"
            "(462.562, strftime('%s', 'now', '-5 minutes'), 'Greystone', 'Order is ready at window two.', 'Drive-thru comms', 'Fast Food Ops'),"
            "(160.230, strftime('%s', 'now', '-1 hour'), 'Rail Yard', 'Train 42 is cleared on track 3.', 'Train clearance', 'Railroad Ch 1');";
        
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

int main() {
    crow::SimpleApp app;
    curl_global_init(CURL_GLOBAL_ALL);

    if (!createTable()) {
        std::cerr << "[API] Database error." << std::endl;
        return 1;
    }

    // 2. Trigger the seed function automatically on startup
    seedDatabase();

    std::cout << "[API] Connecting to Redis..." << std::endl;
    for(int i=0; i<5; i++) {
        g_redis_pub = redisConnect("ag-redis", 6379);
        if (g_redis_pub && !g_redis_pub->err) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!g_redis_pub || g_redis_pub->err) return 1;

    std::thread(redisListenerThread).detach();

    CROW_WEBSOCKET_ROUTE(app, "/ws/audio")
    .onopen([](crow::websocket::connection& conn) { std::lock_guard<std::mutex> lock(ws_audio_mtx); audio_clients.insert(&conn); })
    .onclose([](crow::websocket::connection& conn, const std::string&) { std::lock_guard<std::mutex> lock(ws_audio_mtx); audio_clients.erase(&conn); });

    CROW_WEBSOCKET_ROUTE(app, "/ws/status")
    .onopen([](crow::websocket::connection& conn) { std::lock_guard<std::mutex> lock(ws_status_mtx); status_clients.insert(&conn); })
    .onclose([](crow::websocket::connection& conn, const std::string&) { std::lock_guard<std::mutex> lock(ws_status_mtx); status_clients.erase(&conn); });

    CROW_ROUTE(app, "/api/scan/tune").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        crow::json::wvalue cmd; cmd["command"] = "TUNE"; cmd["freq"] = x["freq"].d();
        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse(200);
    });

    CROW_ROUTE(app, "/api/scan/record").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        crow::json::wvalue cmd; cmd["command"] = "RECORD"; cmd["freq"] = x["freq"].d();
        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "recording_started"}});
    });

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

    CROW_ROUTE(app, "/api/logs")([]() {
        std::vector<RadioLog> logs = getAllLogs();
        crow::json::wvalue res;
        if (logs.empty()) res = crow::json::wvalue::list();
        else {
            for (size_t i = 0; i < logs.size(); i++) {
                res[i]["freq"] = std::to_string(logs[i].freq); res[i]["time"] = logs[i].time;
                res[i]["location"] = logs[i].location; res[i]["name"] = logs[i].channelName;
                res[i]["summary"] = logs[i].summary; res[i]["rawT"] = logs[i].rawT;
            }
        }
        return makeCorsResponse(res);
    });

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

    CROW_ROUTE(app, "/api/dev/seed").methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
    ([]() {
        seedDatabase();
        return makeCorsResponse({{"status", "database_seeded_successfully"}});
    });

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

        insertLog(freq, time, location, rawT, summary, channelName);
        
        return makeCorsResponse({{"status", "success"}});
    });

    CROW_ROUTE(app, "/api/dev/clear").methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
    ([]() {
        sqlite3 *db;
        if (sqlite3_open(DB_NAME, &db) == SQLITE_OK) {
            sqlite3_exec(db, "DELETE FROM RadioLogs;", 0, 0, 0);
            sqlite3_close(db);
        }
        return makeCorsResponse({{"status", "database_wiped_clean"}});
    });

    // ==========================================
    // AI PIPELINE ROUTES (Bridges React to Redis)
    // ==========================================

    CROW_ROUTE(app, "/api/transcribe/local").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}}, 400);

        crow::json::wvalue cmd;
        cmd["command"] = "TRANSCRIBE_LOCAL";
        cmd["freq"] = body["freq"].d();

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH ai_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "transcribing"}});
    });

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

    CROW_ROUTE(app, "/api/transcribe/openai").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return makeCorsResponse({{"status", "error"}}, 400);

        crow::json::wvalue cmd;
        cmd["command"] = "TRANSCRIBE_OPENAI";
        cmd["freq"] = body["freq"].d();

        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH ai_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "transcribing"}});
    });

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

    std::cout << "[API] Gateway online at http://0.0.0.0:8080" << std::endl;
    app.port(8080).multithreaded().run();
    return 0;
}