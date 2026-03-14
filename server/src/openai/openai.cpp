#include "openai.hpp"
#include <iostream>
#include <fstream>
#include <curl/curl.h>
#include <json.hpp>

using json = nlohmann::json;

// --- Private Helper: cURL write callback to capture response data ---
// 'static' keeps this function completely hidden from the rest of your project
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

std::unordered_map<std::string, std::string> loadEnv(const std::string& filename) {
    std::unordered_map<std::string, std::string> envVars;
    std::ifstream file(filename);
    std::string line;
    
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos) {
            std::string key = line.substr(0, delimiterPos);
            std::string value = line.substr(delimiterPos + 1);
            if (!value.empty() && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.length() - 2);
            }
            envVars[key] = value;
        }
    }
    return envVars;
}

std::string transcribeAudio(const std::string& filePath, const std::string& apiKey) {
    CURL* curl = curl_easy_init();
    std::string responseString;

    if (curl) {
        curl_mime* form = curl_mime_init(curl);
        
        // 1. Attach the .wav file
        curl_mimepart* field = curl_mime_addpart(form);
        curl_mime_name(field, "file");
        curl_mime_filedata(field, filePath.c_str());

        // 2. Specify the transcription model
        field = curl_mime_addpart(form);
        curl_mime_name(field, "model");
        curl_mime_data(field, "gpt-4o-mini-transcribe", CURL_ZERO_TERMINATED);

        struct curl_slist* headers = NULL;
        std::string authHeader = "Authorization: Bearer " + apiKey;
        headers = curl_slist_append(headers, authHeader.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/audio/transcriptions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

        CURLcode res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_mime_free(form);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "cURL Error (Transcription): " << curl_easy_strerror(res) << std::endl;
            return "";
        }

        try {
            auto j = json::parse(responseString);
            if (j.contains("text")) {
                return j["text"];
            } else {
                std::cerr << "API Error: " << responseString << std::endl;
            }
        } catch (json::parse_error& e) {
            std::cerr << "JSON Parse Error: " << e.what() << std::endl;
        }
    }
    return "";
}

std::string summarizeText(const std::string& transcript, const std::string& apiKey) {
    if (transcript.empty()) return "";

    CURL* curl = curl_easy_init();
    std::string responseString;

    if (curl) {
        struct curl_slist* headers = NULL;
        std::string authHeader = "Authorization: Bearer " + apiKey;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, authHeader.c_str());

        // Payload configuring gpt-4o-mini and the summarizing prompt
        json payload = {
            {"model", "gpt-4o-mini"},
            {"messages", {
                {{"role", "system"}, {"content", "You are an automated radio intelligence system. Analyze intercepted transcripts and categorize them using exactly one of these labels: [News, Sports, Music, Religion, Talk Radio, Emergency, Advertisement, Unknown]. Provide a strict 1-2 sentence summary of the core subject. Do not include conversational filler. You must format your exact response like this: [Category] - [Summary]."}},
                {{"role", "user"}, {"content", "Transcript:\n\n" + transcript}}
            }}
        };
        std::string payloadStr = payload.dump();

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

        CURLcode res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "cURL Error (GPT): " << curl_easy_strerror(res) << std::endl;
            return "";
        }

        try {
            auto j = json::parse(responseString);
            if (j.contains("choices") && !j["choices"].empty()) {
                return j["choices"][0]["message"]["content"];
            } else {
                std::cerr << "API Error: " << responseString << std::endl;
            }
        } catch (json::parse_error& e) {
            std::cerr << "JSON Parse Error: " << e.what() << std::endl;
        }
    }
    return "";
}