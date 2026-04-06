#ifndef WHISPER_TEST_HPP
#define WHISPER_TEST_HPP

#include <string>
#include <vector>
#include <iostream>
#include "whisper.h" 

class WhisperTest {
public:
    // Initialize with path to model (e.g., "ggml-tiny.bin")
    WhisperTest(const std::string& model_path);
    ~WhisperTest();

    std::string transcribe(const std::string& wav_path);

private:
    struct whisper_context* ctx = nullptr;

    bool read_wav(const std::string& fname, std::vector<float>& pcmf32, std::vector<std::vector<float>>& pcmf32s, bool stereo);
};

#endif 