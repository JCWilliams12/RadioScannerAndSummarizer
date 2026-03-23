#include "whispertinytest.hpp"
#include <fstream>
#include <cstring>
#include <cmath>

// WAV Header struct for parsing
struct wav_header {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
    char fmt_chunk_marker[4];
    uint32_t length_of_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_chunk_header[4];
    uint32_t data_size;
};

WhisperTest::WhisperTest(const std::string& model_path) {
    struct whisper_context_params cparams = whisper_context_default_params();
    ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);

    if (!ctx) {
        std::cerr << "Failed to initialize whisper context with model: " << model_path << std::endl;
    }
}

WhisperTest::~WhisperTest() {
    if (ctx) {
        whisper_free(ctx);
    }
}

std::string WhisperTest::transcribe(const std::string& wav_path) {
    if (!ctx) return "Error: Whisper context not initialized.";

    std::vector<float> pcmf32; // Mono data
    std::vector<std::vector<float>> pcmf32s; // Stereo data (unused here but required by helper)

    // 1. Load WAV file
    if (!read_wav(wav_path, pcmf32, pcmf32s, false)) {
        return "Error: Failed to read WAV file '" + wav_path + "'";
    }

    // 2. Configure full parameters
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_special = false;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.language = "en"; // Set language to English

    // 3. Run the inference
    if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
        return "Error: failed to process audio";
    }

    // 4. Extract text segments
    std::string result = "";
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(ctx, i);
        result += text;
    }

    return result;
}

// Simple WAV loader helper (strips header, converts to float)
bool WhisperTest::read_wav(const std::string& fname, std::vector<float>& pcmf32, std::vector<std::vector<float>>& pcmf32s, bool stereo) {
    std::ifstream file(fname, std::ios::binary);
    if (!file.is_open()) return false;

    wav_header header;
    file.read((char*)&header, sizeof(header));

    if (header.sample_rate != 16000) {
        std::cerr << "Warning: WAV sample rate is " << header.sample_rate << " (expected 16000). Whisper might fail." << std::endl;
    }

    // Read the data
    // Usually 16-bit integers
    int num_samples = header.data_size / (header.channels * (header.bits_per_sample / 8));
    
    // We only support 16-bit PCM for simplicity here
    if (header.bits_per_sample != 16) {
        std::cerr << "Error: Only 16-bit PCM WAV files supported." << std::endl;
        return false;
    }

    pcmf32.resize(num_samples);
    
    for (int i = 0; i < num_samples; ++i) {
        int16_t sample;
        file.read((char*)&sample, sizeof(sample));
        // Convert to float [-1.0, 1.0]
        pcmf32[i] = (float)sample / 32768.0f;
        
        if (header.channels == 2) {
             // Skip the second channel for mono processing if needed
            int16_t sample2;
            file.read((char*)&sample2, sizeof(sample2));
        }
    }
    
    return true;
}


/*  usage:
    std::string base_folder = "server/src/whispertinytest/";
    std::string model_path  = base_folder + "ggml-base.en.bin";
    std::string wav_path    = base_folder + "audio.wav";

    std::cout << "[INFO] Loading model..." << std::endl;
    WhisperTest transcriber(model_path);
    
    std::cout << "[INFO] Transcribing " << wav_path << "..." << std::endl;
    std::string text = transcriber.transcribe(wav_path);
*/