#pragma once

#include <string>
#include <fstream>
#include <vector>
#include <cstdint>
#include <mutex>     
#include <atomic>

class IqToWav {
public:
    IqToWav(int inputSampleRate, int outputSampleRate, std::string filename);
    ~IqToWav();

    void ProcessBlock(short *xi, short *xq, unsigned int numSamples);
    void StartRecording();
    void StopRecording();

private:
    int inputRate;
    int outputRate;
    int decimation;
    std::string outFilename;
    std::ofstream wavFile;

    std::atomic<bool> isRecording; 
    std::mutex fileMutex;
    uint32_t totalAudioBytes;

    // Filter Tracking States
    float prev_phase = 0.0f;
    float aa_state = 0.0f;       // Anti-aliasing accumulator
    float de_state = 0.0f;       // De-emphasis accumulator
    float audioAccum = 0.0f;     
    int decimationCounter = 0;

    void WriteWavHeader();
    void FinalizeWavHeader();
};