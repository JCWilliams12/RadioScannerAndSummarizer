#include <iostream>
#include <hiredis/hiredis.h>
#include <cstdlib>
#include "crow.h"
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
        if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            auto json = crow::json::load(reply->element[2]->str);
            if (!json) {
                freeReplyObject(reply);
                continue; 
            }

            std::string cmd = json["command"].s();
            double targetFreq = json["freq"].d();

            // --- THE FINAL FIX ---
            // Extract the dynamic filename from the JSON, default to audio.wav if missing
            std::string filename = json.has("file") ? std::string(json["file"].s()) : "audio.wav";
            std::string audioFilePath = "/app/shared/audio/" + filename;

            std::cout << "\n[AI Worker] Received command: " << cmd << " for " << targetFreq << " MHz" << std::endl;
            std::cout << "[AI Worker] Target audio file: " << audioFilePath << std::endl;

            if (cmd == "TRANSCRIBE_LOCAL") {
                std::cout << "[AI Worker] Starting Local Whisper..." << std::endl;
                WhisperTest transcriber("/app/shared/models/ggml-base.en.bin");
                
                // Pass the dynamic path to the transcriber!
                std::string text = transcriber.transcribe(audioFilePath); 
                
                std::cout << "[AI Worker] Local Whisper Output: " << text << std::endl;
                
                crow::json::wvalue msg; 
                msg["event"] = "transcription_complete"; msg["text"] = text; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            } 
            else if (cmd == "SUMMARIZE_LOCAL") {
                std::cout << "[AI Worker] Starting Local Phi-3..." << std::endl;
                std::string summary = GenerateSummary(json["text"].s());
                std::cout << "[AI Worker] Local Phi-3 Output: " << summary << std::endl;
                
                crow::json::wvalue msg; 
                msg["event"] = "summary_complete"; msg["summary"] = summary; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
            else if (cmd == "TRANSCRIBE_OPENAI") {
                std::cout << "[AI Worker] Sending audio to OpenAI API..." << std::endl;
                
                // Pass the dynamic path to the OpenAI API!
                std::string text = transcribeAudio(audioFilePath, getEnvVar("OPENAI_API_KEY"));
                
                std::cout << "[AI Worker] OpenAI API Response: " << text << std::endl;
                
                crow::json::wvalue msg; 
                msg["event"] = "transcription_complete"; msg["text"] = text; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
            else if (cmd == "SUMMARIZE_OPENAI") {
                std::cout << "[AI Worker] Sending text to OpenAI API..." << std::endl;
                std::string summary = summarizeText(json["text"].s(), getEnvVar("OPENAI_API_KEY"));
                std::cout << "[AI Worker] OpenAI API Response: " << summary << std::endl;
                
                crow::json::wvalue msg; 
                msg["event"] = "summary_complete"; msg["summary"] = summary; msg["freq"] = targetFreq;
                redisCommand(pub, "PUBLISH ws_updates %s", msg.dump().c_str());
            }
        }
        if (reply) freeReplyObject(reply);
    }
    return 0;
}