#define _USE_MATH_DEFINES
#include <cmath>
#include <iostream>
#include "iqtowav.hpp"

extern void broadcastAudio(const std::vector<int16_t>& audioBuffer);

IqToWav::IqToWav(int inputSampleRate, int outputSampleRate, std::string filename)
    : inputRate(inputSampleRate), outputRate(outputSampleRate),
      outFilename(filename), isRecording(false), totalAudioBytes(0) {
    decimation = inputRate / outputRate;
}

IqToWav::~IqToWav() {
    StopRecording();
}

void IqToWav::StartRecording() {
    std::lock_guard<std::mutex> lock(fileMutex);
    wavFile.open(outFilename, std::ios::binary);
    if (!wavFile.is_open()) {
        std::cerr << "Failed to open WAV file for writing!" << std::endl;
        return;
    }
    WriteWavHeader();
    totalAudioBytes = 0;
    isRecording = true; 
    std::cout << "Started recording to " << outFilename << std::endl;
}

void IqToWav::StopRecording() {
    isRecording = false; 
    std::lock_guard<std::mutex> lock(fileMutex); 
    if (wavFile.is_open()) {
        FinalizeWavHeader();
        wavFile.close();
        std::cout << "Finished recording. Saved " << totalAudioBytes / outputRate / 2 << " seconds of audio." << std::endl;
    }
}

void IqToWav::ProcessBlock(short *xi, short *xq, unsigned int numSamples) {
    std::vector<int16_t> audioBuffer;
    audioBuffer.reserve((numSamples / decimation) + 1);

    const float FM_GAIN = 133505.0f; 
    const float aa_alpha = 0.0245f;  // 8kHz Low-Pass Filter (Anti-Aliasing at 2.0MHz)
    const float de_alpha = 0.4545f;  // 75us Low-Pass Filter (De-emphasis at 16kHz)

    for (unsigned int i = 0; i < numSamples; i++) {
        // 1. Direct Baseband Mapping (Zero-IF)
        float bb_i = static_cast<float>(xi[i]);
        float bb_q = static_cast<float>(xq[i]);

        // 2. FM Demodulation
        float current_phase = std::atan2(bb_q, bb_i);
        float phase_diff = current_phase - prev_phase;

        if (phase_diff >  M_PI) phase_diff -= 2.0f * M_PI;
        if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
        prev_phase = current_phase;

        // 3. THE FIX: Anti-Aliasing Filter
        // Strips away high-frequency RF noise before we compress the data
        aa_state = (aa_alpha * phase_diff) + ((1.0f - aa_alpha) * aa_state);

        audioAccum += aa_state;
        decimationCounter++;

        // 4. Decimate to 16kHz and apply De-emphasis
        if (decimationCounter >= decimation) {
            float raw_audio = (audioAccum / static_cast<float>(decimation)) * FM_GAIN;

            // Apply US Standard 75us curve to remove the FM hiss
            de_state = (de_alpha * raw_audio) + ((1.0f - de_alpha) * de_state);

            float audio_float = de_state;

            // Clip to prevent integer overflow clipping (loud pops)
            if (audio_float >  32767.0f) audio_float =  32767.0f;
            if (audio_float < -32768.0f) audio_float = -32768.0f;

            audioBuffer.push_back(static_cast<int16_t>(audio_float));

            audioAccum = 0.0f;
            decimationCounter = 0;
        }
    }

    if (!audioBuffer.empty()) {
        broadcastAudio(audioBuffer);

        if (isRecording) {
            std::lock_guard<std::mutex> lock(fileMutex);
            if (wavFile.is_open()) {
                wavFile.write(reinterpret_cast<const char*>(audioBuffer.data()), audioBuffer.size() * sizeof(int16_t));
                totalAudioBytes += audioBuffer.size() * sizeof(int16_t);
            }
        }
    }
}

void IqToWav::WriteWavHeader() {
    wavFile.write("RIFF", 4);
    uint32_t placeholder = 0;
    wavFile.write(reinterpret_cast<char*>(&placeholder), 4);
    wavFile.write("WAVE", 4);
    wavFile.write("fmt ", 4);

    uint32_t fmtSize       = 16;
    uint16_t audioFormat   = 1;
    uint16_t numChannels   = 1;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate      = (uint32_t)outputRate * 2;
    uint16_t blockAlign    = 2;
    uint32_t outRateInt    = (uint32_t)outputRate;

    wavFile.write(reinterpret_cast<char*>(&fmtSize),       4);
    wavFile.write(reinterpret_cast<char*>(&audioFormat),   2);
    wavFile.write(reinterpret_cast<char*>(&numChannels),   2);
    wavFile.write(reinterpret_cast<char*>(&outRateInt),    4);
    wavFile.write(reinterpret_cast<char*>(&byteRate),      4);
    wavFile.write(reinterpret_cast<char*>(&blockAlign),    2);
    wavFile.write(reinterpret_cast<char*>(&bitsPerSample), 2);

    wavFile.write("data", 4);
    wavFile.write(reinterpret_cast<char*>(&placeholder), 4);
}

void IqToWav::FinalizeWavHeader() {
    wavFile.seekp(4, std::ios::beg);
    uint32_t fileSize = 36 + totalAudioBytes;
    wavFile.write(reinterpret_cast<char*>(&fileSize), 4);

    wavFile.seekp(40, std::ios::beg);
    wavFile.write(reinterpret_cast<char*>(&totalAudioBytes), 4);
}