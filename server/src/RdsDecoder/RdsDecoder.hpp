#pragma once
#include <vector>
#include <string>

class RdsDecoder {
public:
    // Main entry point for the scanner
    static std::string ExtractName(const std::vector<float>& iq_buffer, float sampleRate);

private:
    // Step 1: Convert raw I/Q radio waves into a 1D audio waveform
    static std::vector<float> FmDemodulate(const std::vector<float>& iq_buffer);
    
    // Step 2: Strip away normal music/voices to leave only the 57 kHz data tone
    static std::vector<float> BandpassFilter57kHz(const std::vector<float>& fm_baseband, float sampleRate);
    
    // Step 3: Track the phase shifts of the tone to extract 1s and 0s
    static std::vector<int> BpskCostasLoop(const std::vector<float>& rds_tone, float sampleRate);
    
    // Step 4: Parse the 1s and 0s into the 8-character Station Name
    static std::string DecodeBitsToText(const std::vector<int>& bits);
};