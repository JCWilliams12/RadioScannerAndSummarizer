#include <iostream>
#include <hiredis/hiredis.h>
#include <cstdlib>
#include "crow.h"

// Include your existing headers
#include "ollamatest.hpp" 
#include "whispertinytest.hpp"
#include "openai.hpp"

// Utility function to get the API key
std::string getEnvVar(std::string key) {
    char * val = getenv(key.c_str());
    return val == NULL ? std::string("") : std::string(val);
}

int main() {
    std::cout << "[AI Worker] Online and waiting..." << std::endl;
    redisContext *c = redisConnect("ag-redis", 6379);
    redisContext *pub = redisConnect("ag-redis", 6379);

    redisReply *reply = (redisReply*)redisCommand(c, "SUBSCRIBE ai_commands");
    freeReplyObject(reply);

    while (redisGetReply(c, (void**)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            std::string cmd = json["command"].s();
            double targetFreq = json["freq"].d();

            if (cmd == "TRANSCRIBE_LOCAL") {
                WhisperTest transcriber("/app/shared/models/ggml-base.en.bin");
                std::string text = transcriber.transcribe("/app/shared/audio/audio.wav");
                
                crow::json::wvalue msg; 
                msg["event"] = "transcription_complete"; msg["text"] = text; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            } 
            else if (cmd == "SUMMARIZE_LOCAL") {
                // Ensure your GenerateSummary function points to "/app/shared/models/Phi-3-mini-4k-instruct-q4.gguf"
                std::string summary = GenerateSummary(json["text"].s());
                
                crow::json::wvalue msg; 
                msg["event"] = "summary_complete"; msg["summary"] = summary; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
            else if (cmd == "TRANSCRIBE_OPENAI") {
                std::string text = transcribeAudio("/app/shared/audio/audio.wav", getEnvVar("OPENAI_API_KEY"));
                
                crow::json::wvalue msg; 
                msg["event"] = "transcription_complete"; msg["text"] = text; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
            else if (cmd == "SUMMARIZE_OPENAI") {
                std::string summary = summarizeText(json["text"].s(), getEnvVar("OPENAI_API_KEY"));
                
                crow::json::wvalue msg; 
                msg["event"] = "summary_complete"; msg["summary"] = summary; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
        }
        freeReplyObject(reply);
    }
    return 0;
}