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

std::mutex ws_audio_mtx;
std::unordered_set<crow::websocket::connection*> audio_clients;
std::mutex ws_status_mtx;
std::unordered_set<crow::websocket::connection*> status_clients;

redisContext *g_redis_pub = nullptr;
std::mutex g_redis_pub_mtx;

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

    std::cout << "[API] Connecting to Redis..." << std::endl;
    for(int i=0; i<5; i++) {
        g_redis_pub = redisConnect("ag-redis", 6379);
        if (g_redis_pub && !g_redis_pub->err) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!g_redis_pub || g_redis_pub->err) return 1;

    std::thread(redisListenerThread).detach();

    CROW_WEBSOCKET_ROUTE(app, "/ws/audio")
    .onopen([&](crow::websocket::connection& conn) { std::lock_guard<std::mutex> lock(ws_audio_mtx); audio_clients.insert(&conn); })
    .onclose([&](crow::websocket::connection& conn, const std::string&) { std::lock_guard<std::mutex> lock(ws_audio_mtx); audio_clients.erase(&conn); });

    CROW_WEBSOCKET_ROUTE(app, "/ws/status")
    .onopen([&](crow::websocket::connection& conn) { std::lock_guard<std::mutex> lock(ws_status_mtx); status_clients.insert(&conn); })
    .onclose([&](crow::websocket::connection& conn, const std::string&) { std::lock_guard<std::mutex> lock(ws_status_mtx); status_clients.erase(&conn); });

    CROW_ROUTE(app, "/api/scan/tune").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        crow::json::wvalue cmd; cmd["command"] = "TUNE"; cmd["freq"] = x["freq"].d();
        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", "tuned"}});
    });

    // FIX: New route to start and stop live listen mode.
    // Previously the frontend had no way to send a LIVE_LISTEN or STOP_LIVE
    // command to the SDR daemon, so g_mode never entered LIVE_LISTEN and
    // dspWorker never published PCM to Redis. The WebSocket received silence.
    CROW_ROUTE(app, "/api/scan/live").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req) {
        auto x = crow::json::load(req.body); if (!x) return crow::response(400);
        std::string action = x["action"].s(); // "start" or "stop"
        crow::json::wvalue cmd;
        cmd["command"] = (action == "start") ? "LIVE_LISTEN" : "STOP_LIVE";
        std::lock_guard<std::mutex> lock(g_redis_pub_mtx);
        redisCommand(g_redis_pub, "PUBLISH sdr_commands %s", cmd.dump().c_str());
        return makeCorsResponse({{"status", action == "start" ? "live_started" : "live_stopped"}});
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

    CROW_ROUTE(app, "/api/search")
    ([](const crow::request& req) {
        std::string q = req.url_params.get("q") ? req.url_params.get("q") : "";
        std::vector<RadioLog> logs;
        try { logs = filterByFrequency(std::stod(q)); } catch (...) { logs = filterByLocation(q); }
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

    CROW_ROUTE(app, "/stations")([]() { return makeCorsResponse(crow::json::wvalue::list()); });

    std::cout << "[API] Gateway online at http://0.0.0.0:8080" << std::endl;
    app.port(8080).multithreaded().run();
    return 0;
}