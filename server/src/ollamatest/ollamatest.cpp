#include "ollama.hpp"
#include "ollamatest.hpp"
#include <string>
#include <iostream>

std::string GenerateSummary(std::string transcript) {

    try {
    
    std::string prompt = "Analyze the following intercepted radio transcript. "
                         "Categorize it using exactly one of these labels: "
                         "[News, Sports, Music, Religion, Talk Radio, Emergency, Advertisement, Unknown]. "
                         "Provide a strict 1-2 sentence summary of the core subject. "
                         "Do not include conversational filler. Format your exact response like this: "
                         "[Category] - [Summary].\n\n"
                         "Transcript: " + transcript;


    ollama::response response = ollama::generate("phi3:mini", prompt);


    std::stringstream ss;
    ss << response;
    
    return ss.str();
    
} catch (const std::exception& e) {
    std::cerr << "\n[CRITICAL ERROR] Ollama failed: " << e.what() << std::endl;
} catch (...) {
    std::cerr << "\n[CRITICAL ERROR] Unknown failure occurred." << std::endl;
}

    
}