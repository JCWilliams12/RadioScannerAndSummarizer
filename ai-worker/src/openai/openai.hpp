#ifndef OPENAI_HPP
#define OPENAI_HPP
#include <string>
#include <unordered_map>

std::unordered_map<std::string, std::string> loadEnv(const std::string& filename);
std::string transcribeAudio(const std::string& filePath, const std::string& apiKey);
std::string summarizeText(const std::string& transcript, const std::string& apiKey);

#endif