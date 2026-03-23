#ifndef WHISPER_TEST_HPP
#define WHISPER_TEST_HPP

#include <string>
#include <vector>
#include <iostream>
#include "whisper.h" // Available because of FetchContent

class WhisperTest {
public:
    // Initialize with path to model (e.g., "ggml-tiny.bin")
    WhisperTest(const std::string& model_path);
    ~WhisperTest();

    // The main method you asked for
    std::string transcribe(const std::string& wav_path);

private:
    struct whisper_context* ctx = nullptr;

    // Helper to read WAV file into float array
    bool read_wav(const std::string& fname, std::vector<float>& pcmf32, std::vector<std::vector<float>>& pcmf32s, bool stereo);
};

#endif // WHISPER_TEST_HPP